#pragma once

#include "photobridge/common/status.h"
#include "photobridge/model/directory_entry.h"

namespace photobridge {

class DirectoryEntrySink {
public:
    virtual Status Add(DirectoryEntry entry) = 0;
    virtual ~DirectoryEntrySink() = default;
};

class DirectoryWalker {
public:
    virtual Status Walk(
        int root_fd,
        DirectoryEntrySink& sink) = 0;
    virtual ~DirectoryWalker() = default;
};

}  // namespace photobridge
