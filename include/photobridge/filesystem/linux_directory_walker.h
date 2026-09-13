#pragma once

#include "photobridge/filesystem/directory_walker.h"

namespace photobridge {

class LinuxDirectoryWalker final : public DirectoryWalker {
public:
    Status Walk(
        int root_fd,
        DirectoryEntrySink& sink) override;
};

}  // namespace photobridge
