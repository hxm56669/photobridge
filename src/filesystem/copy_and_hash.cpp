#include "photobridge/filesystem/copy_and_hash.h"

#include <blake3.h>

#include <cstdint>
#include <limits>

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

    auto before = file_ops.StatFd(source_fd);
    if (!before.ok()) {
        return before.status();
    }

    CopyResult result;
    result.source_before = before.value();
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
        result.bytes_copied += read.value();
    }

    auto after = file_ops.StatFd(source_fd);
    if (!after.ok()) {
        return after.status();
    }
    result.source_after = after.value();
    if (result.source_before != result.source_after) {
        return Status(
            StatusCode::kInternal,
            "source file identity changed during copy");
    }

    auto digest = hasher.Finalize();
    if (!digest.ok()) {
        return digest.status();
    }
    result.source_digest = digest.value();
    return result;
}

}  // namespace photobridge
