#include "photobridge/filesystem/copy_and_hash.h"

#include <blake3.h>

#include <cstdint>
#include <limits>

#include "photobridge/common/test_hooks.h"

namespace photobridge {
namespace {

Status Invalid(const char* message)
{
    return Status(StatusCode::kInvalidArgument, message);
}

Status WriteAll(
    FileOps& file_ops,
    int target_fd,
    std::span<const std::byte> bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto written = file_ops.Write(target_fd, bytes.subspan(offset));
        if (!written.ok()) {
            return written.status();
        }
        if (written.value() == 0) {
            return Status(
                StatusCode::kIoError,
                "target write made no progress");
        }
        if (written.value() > bytes.size() - offset) {
            return Status(
                StatusCode::kInternal,
                "target write exceeded requested byte count");
        }
        offset += written.value();
    }
    return Status::Ok();
}

}  // namespace

struct Blake3Hasher::State {
    blake3_hasher hasher{};
    bool finalized = false;
};

Blake3Hasher::Blake3Hasher()
    : state_(new State())
{
    blake3_hasher_init(&state_->hasher);
}

Blake3Hasher::~Blake3Hasher()
{
    delete state_;
}

Status Blake3Hasher::Update(std::span<const std::byte> bytes)
{
    if (state_->finalized) {
        return Invalid("cannot update a finalized hasher");
    }
    blake3_hasher_update(
        &state_->hasher,
        bytes.data(),
        bytes.size());
    return Status::Ok();
}

StatusOr<Digest> Blake3Hasher::Finalize()
{
    if (state_->finalized) {
        return Invalid("hasher was already finalized");
    }
    state_->finalized = true;
    Digest digest;
    blake3_hasher_finalize(
        &state_->hasher,
        reinterpret_cast<std::uint8_t*>(digest.bytes.data()),
        digest.bytes.size());
    return digest;
}

StatusOr<CopyResult> CopyAndHash(
    FileOps& file_ops,
    Hasher& hasher,
    int source_fd,
    int target_fd,
    std::span<std::byte> buffer)
{
    if (buffer.empty()) {
        return Invalid("copy buffer must not be empty");
    }

    CopyResult result;
    bool paused_after_first_write = false;
    for (;;) {
        auto read = file_ops.Read(source_fd, buffer);
        if (!read.ok()) {
            return read.status();
        }
        if (read.value() > buffer.size()) {
            return Status(
                StatusCode::kInternal,
                "source read exceeded buffer capacity");
        }
        if (read.value() == 0) {
            break;
        }
        if (result.bytes_copied > std::numeric_limits<std::uint64_t>::max()
                - read.value()) {
            return Status(
                StatusCode::kInternal,
                "copied byte count overflowed");
        }

        const std::span<const std::byte> chunk(buffer.data(), read.value());
        Status status = hasher.Update(chunk);
        if (!status.ok()) {
            return status;
        }
        status = WriteAll(file_ops, target_fd, chunk);
        if (!status.ok()) {
            return status;
        }
        if (!paused_after_first_write) {
            PauseForTest("PHOTOBRIDGE_TEST_PAUSE_AFTER_FIRST_COPY_WRITE_MS");
            paused_after_first_write = true;
        }
        result.bytes_copied += read.value();
    }

    auto digest = hasher.Finalize();
    if (!digest.ok()) {
        return digest.status();
    }
    result.source_digest = digest.value();
    return result;
}

StatusOr<CopyResult> CopyAndHash(
    FileOps& file_ops,
    Hasher& hasher,
    MutationGuard& source_guard,
    int target_fd,
    std::span<std::byte> buffer)
{
    Status status = source_guard.VerifyBeforeRead();
    if (!status.ok()) return status;
    auto result = CopyAndHash(
        file_ops,
        hasher,
        source_guard.fd(),
        target_fd,
        buffer);
    if (!result.ok()) return result.status();
    status = source_guard.VerifyAfterRead();
    if (!status.ok()) return status;
    return result;
}

}  // namespace photobridge
