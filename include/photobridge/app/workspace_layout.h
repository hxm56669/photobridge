#pragma once

#include <filesystem>

#include "photobridge/common/status_or.h"

namespace photobridge {

struct WorkspaceLayout {
    std::filesystem::path root;
    std::filesystem::path database;
    std::filesystem::path manifests;
    std::filesystem::path plans;
    std::filesystem::path reports;
    std::filesystem::path logs;
    std::filesystem::path locks;

    static StatusOr<WorkspaceLayout> FromRoot(
        std::filesystem::path root);
};

}  // namespace photobridge
