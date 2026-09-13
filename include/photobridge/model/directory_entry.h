#pragma once

#include "photobridge/model/file_identity.h"
#include "photobridge/model/relative_path.h"

namespace photobridge {

enum class DirectoryEntryKind {
    kRegularFile,
    kDirectory,
    kSymlink,
    kOther,
};

struct DirectoryEntry {
    RelativePath relative_path;
    FileIdentity identity;
    DirectoryEntryKind kind = DirectoryEntryKind::kOther;
};

}  // namespace photobridge
