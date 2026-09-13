#include "photobridge/filesystem/binary_verifier.h"

#include <limits>

#include "photobridge/filesystem/copy_and_hash.h"

namespace photobridge {
namespace {

Status Invalid(const char* message)
{
    return Status(StatusCode::kInvalidArgument, message);
}

}  // namespace

StatusOr<BinaryVerificationResult> VerifyBinary(
    FileOps& file_ops,
    Hasher& hasher,
    int target_fd,
    std::uint64_t expected_size,
    const std::optional<Digest>& expected_digest,
    std::span<std::byte> buffer)
{
    if (buffer.empty()) {
        return Invalid("verification buffer must not be empty");
    }

    auto before = file_ops.StatFd(target_fd);
    if (!before.ok()) return before.status();

    BinaryVerificationResult result;
    result.target_before = before.value();
    for (;;) {
        auto read = file_ops.Read(target_fd, buffer);
        if (!read.ok()) return read.status();
        if (read.value() > buffer.size()) {
            return Status(
                StatusCode::kInternal,
                "target read exceeded buffer capacity");
        }
        if (read.value() == 0) break;
        if (result.bytes_read > std::numeric_limits<std::uint64_t>::max()
                - read.value()) {
            return Status(
                StatusCode::kInternal,
                "verified byte count overflowed");
        }
        Status status = hasher.Update(
            std::span<const std::byte>(buffer.data(), read.value()));
        if (!status.ok()) return status;
        result.bytes_read += read.value();
    }

    auto after = file_ops.StatFd(target_fd);
    if (!after.ok()) return after.status();
    result.target_after = after.value();
    if (result.target_before != result.target_after) {
        return Status(
            StatusCode::kInternal,
            "target file identity changed during verification");
    }

    auto digest = hasher.Finalize();
    if (!digest.ok()) return digest.status();
    result.target_digest = digest.value();
    if (!expected_digest.has_value()) {
        result.status = BinaryVerification::kNotApplicable;
    } else if (result.bytes_read == expected_size
               && result.target_digest == expected_digest.value()) {
        result.status = BinaryVerification::kIdentical;
    } else {
        result.status = BinaryVerification::kMismatch;
    }
    return result;
}

}  // namespace photobridge
