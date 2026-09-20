#include "photobridge/filesystem/io_uring_copy_engine.h"

#include <liburing.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <string>
#include <vector>

#include "photobridge/common/posix_error.h"
#include "photobridge/common/test_hooks.h"

namespace photobridge {
namespace {

constexpr std::size_t kBufferCount = 4;

class IoUringContext final {
public:
    explicit IoUringContext(unsigned entries) noexcept
        : result_(io_uring_queue_init(entries, &ring_, 0)) {}

    ~IoUringContext()
    {
        if (result_ == 0) io_uring_queue_exit(&ring_);
    }

    IoUringContext(const IoUringContext&) = delete;
    IoUringContext& operator=(const IoUringContext&) = delete;

    int result() const noexcept { return result_; }
    io_uring* get() noexcept { return &ring_; }

private:
    io_uring ring_{};
    int result_;
};

enum class Phase { kFree, kReading, kReady, kWriting };

struct BufferSlot {
    std::vector<std::byte> bytes;
    std::uint64_t sequence = 0;
    std::uint64_t offset = 0;
    std::size_t length = 0;
    std::size_t progress = 0;
    Phase phase = Phase::kFree;
};

Status Invalid(const char* message)
{
    return Status(StatusCode::kInvalidArgument, message);
}

Status IoError(int result, const char* operation)
{
    return StatusFromErrno(-result, operation);
}

}  // namespace

StatusOr<std::optional<CopyResult>> TryIoUringCopyAndHash(
    Hasher& hasher,
    int source_fd,
    int target_fd,
    std::uint64_t source_size,
    std::size_t chunk_size)
{
    if (source_fd < 0 || target_fd < 0 || chunk_size == 0
        || chunk_size > std::numeric_limits<unsigned>::max()
        || source_size > static_cast<std::uint64_t>(
               std::numeric_limits<off_t>::max())) {
        return Invalid("invalid io_uring copy descriptors, size, or chunk size");
    }

    IoUringContext context(8);
    if (context.result() != 0) {
        if (context.result() == -ENOSYS || context.result() == -EPERM
            || context.result() == -EOPNOTSUPP || context.result() == -EINVAL) {
            return std::optional<CopyResult>{};
        }
        return IoError(context.result(), "initialize io_uring copy queue");
    }

    CopyResult result;
    const std::uint64_t chunk_count = source_size / chunk_size
        + (source_size % chunk_size != 0);
    const std::size_t slot_count = static_cast<std::size_t>(
        std::min<std::uint64_t>(kBufferCount, chunk_count));
    std::vector<BufferSlot> slots(slot_count);
    for (auto& slot : slots) slot.bytes.resize(chunk_size);

    std::uint64_t next_read = 0;
    std::uint64_t next_hash = 0;
    std::uint64_t completed = 0;
    std::size_t in_flight = 0;
    bool paused_after_first_write = false;
    Status failure = Status::Ok();

    const auto submit = [&]() -> Status {
        const int submitted = io_uring_submit(context.get());
        if (submitted < 0) return IoError(submitted, "submit io_uring copy I/O");
        if (submitted == 0) {
            return Status(StatusCode::kInternal, "io_uring submitted no I/O");
        }
        ++in_flight;
        return Status::Ok();
    };

    const auto submit_request = [&](std::size_t index, bool write) -> Status {
        BufferSlot& slot = slots[index];
        io_uring_sqe* sqe = io_uring_get_sqe(context.get());
        if (sqe == nullptr) {
            return Status(StatusCode::kInternal, "io_uring copy queue is full");
        }
        void* data = slot.bytes.data() + slot.progress;
        const unsigned remaining = static_cast<unsigned>(
            slot.length - slot.progress);
        const std::uint64_t offset = slot.offset + slot.progress;
        if (write) {
            io_uring_prep_write(sqe, target_fd, data, remaining, offset);
            slot.phase = Phase::kWriting;
        } else {
            io_uring_prep_read(sqe, source_fd, data, remaining, offset);
            slot.phase = Phase::kReading;
        }
        io_uring_sqe_set_data64(sqe, index * 2 + (write ? 1 : 0));
        return submit();
    };

    const auto start_read = [&](std::size_t index) -> Status {
        BufferSlot& slot = slots[index];
        slot.sequence = next_read++;
        slot.offset = slot.sequence * chunk_size;
        slot.length = static_cast<std::size_t>(std::min<std::uint64_t>(
            chunk_size, source_size - slot.offset));
        slot.progress = 0;
        return submit_request(index, false);
    };

    const auto hash_ready = [&]() -> Status {
        while (next_hash < chunk_count) {
            std::size_t index = slots.size();
            for (std::size_t candidate = 0; candidate < slots.size(); ++candidate) {
                if (slots[candidate].phase == Phase::kReady
                    && slots[candidate].sequence == next_hash) {
                    index = candidate;
                    break;
                }
            }
            if (index == slots.size()) break;
            BufferSlot& slot = slots[index];
            Status status = hasher.Update(
                std::span<const std::byte>(slot.bytes.data(), slot.length));
            if (!status.ok()) return status;
            ++next_hash;
            slot.progress = 0;
            status = submit_request(index, true);
            if (!status.ok()) return status;
        }
        return Status::Ok();
    };

    for (std::size_t index = 0; index < slot_count; ++index) {
        failure = start_read(index);
        if (!failure.ok()) break;
    }

    while (in_flight != 0) {
        io_uring_cqe* cqe = nullptr;
        const int waited = io_uring_wait_cqe(context.get(), &cqe);
        if (waited == -EINTR) continue;
        if (waited < 0) {
            return IoError(waited, "wait for io_uring copy completion");
        }

        const std::uint64_t user_data = io_uring_cqe_get_data64(cqe);
        const std::size_t index = static_cast<std::size_t>(user_data / 2);
        const bool write = (user_data & 1U) != 0;
        const int transferred = cqe->res;
        io_uring_cqe_seen(context.get(), cqe);
        --in_flight;
        if (!failure.ok()) continue;  // Drain in-flight requests before freeing buffers.
        if (index >= slots.size()) {
            failure = Status(StatusCode::kInternal, "invalid io_uring copy completion");
            continue;
        }
        BufferSlot& slot = slots[index];
        if (slot.phase != (write ? Phase::kWriting : Phase::kReading)) {
            failure = Status(StatusCode::kInternal, "unexpected io_uring copy phase");
            continue;
        }
        if (transferred < 0) {
            failure = IoError(transferred, write
                ? "write io_uring copy chunk" : "read io_uring copy chunk");
            continue;
        }
        if (transferred == 0
            || static_cast<std::size_t>(transferred)
                > slot.length - slot.progress) {
            failure = Status(StatusCode::kIoError, write
                ? "io_uring copy write made no progress"
                : "io_uring copy source ended before expected size");
            continue;
        }
        slot.progress += static_cast<std::size_t>(transferred);
        if (slot.progress < slot.length) {
            failure = submit_request(index, write);
            continue;
        }

        if (write) {
            slot.phase = Phase::kFree;
            result.bytes_copied += slot.length;
            ++completed;
            if (!paused_after_first_write) {
                PauseForTest("PHOTOBRIDGE_TEST_PAUSE_AFTER_FIRST_COPY_WRITE_MS");
                paused_after_first_write = true;
            }
            if (next_read < chunk_count) failure = start_read(index);
        } else {
            slot.phase = Phase::kReady;
        }
        if (failure.ok()) failure = hash_ready();
    }

    if (!failure.ok()) return failure;
    if (completed != chunk_count || next_hash != chunk_count
        || result.bytes_copied != source_size) {
        return Status(StatusCode::kInternal, "io_uring copy ended early");
    }
    auto digest = hasher.Finalize();
    if (!digest.ok()) return digest.status();
    result.source_digest = digest.value();
    return std::optional<CopyResult>(result);
}

}  // namespace photobridge
