#pragma once

#include <cstddef>
#include <span>
#include <string>

#include "photobridge/common/status.h"
#include "photobridge/common/status_or.h"
#include "photobridge/filesystem/copy_and_hash.h"

namespace photobridge {

struct TempCommitResult {
    CopyResult copy;
    std::string temp_name;
    std::string final_name;
};

// Copies source_fd into a newly-created sibling temp file, makes that file
// durable, then publishes it with no-replace rename and syncs the directory.
// A rename failure intentionally leaves the temp file for recovery inspection.
StatusOr<TempCommitResult> CopyToTempAndPublish(
    FileOps& file_ops,
    Hasher& hasher,
    int source_fd,
    int target_parent_fd,
    std::string temp_name,
    std::string final_name,
    std::span<std::byte> buffer,
    mode_t mode = 0600);

}  // namespace photobridge
