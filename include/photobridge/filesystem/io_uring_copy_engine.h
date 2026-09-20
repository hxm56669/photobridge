#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "photobridge/filesystem/copy_and_hash.h"

namespace photobridge {

// Returns nullopt only when io_uring is unavailable before any I/O starts.
// Once a request is submitted, errors are returned without retrying the copy.
StatusOr<std::optional<CopyResult>> TryIoUringCopyAndHash(
    Hasher& hasher,
    int source_fd,
    int target_fd,
    std::uint64_t source_size,
    std::size_t chunk_size = 1024U * 1024U);

}  // namespace photobridge
