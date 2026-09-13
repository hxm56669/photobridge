#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "photobridge/common/digest.h"
#include "photobridge/common/status.h"
#include "photobridge/common/status_or.h"
#include "photobridge/filesystem/file_ops.h"
#include "photobridge/model/file_identity.h"

namespace photobridge {

class Hasher;

enum class BinaryVerification {
    kIdentical,
    kMismatch,
    kNotApplicable,
    kUnverifiable,
};

struct BinaryVerificationResult {
    BinaryVerification status = BinaryVerification::kUnverifiable;
    std::uint64_t bytes_read = 0;
    Digest target_digest;
    FileIdentity target_before;
    FileIdentity target_after;
};

// Reads and hashes the target fd independently of the source-copy stream.
// The result is NOT_APPLICABLE when no frozen expected digest is supplied.
StatusOr<BinaryVerificationResult> VerifyBinary(
    FileOps& file_ops,
    Hasher& hasher,
    int target_fd,
    std::uint64_t expected_size,
    const std::optional<Digest>& expected_digest,
    std::span<std::byte> buffer);

}  // namespace photobridge
