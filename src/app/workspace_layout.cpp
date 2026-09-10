#include "photobridge/app/workspace_layout.h"

#include <utility>

namespace photobridge {

StatusOr<WorkspaceLayout> WorkspaceLayout::FromRoot(
    std::filesystem::path root)
{
    if (root.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "workspace root must not be empty");
    }

    WorkspaceLayout layout;
    layout.root = std::move(root);
    layout.database = layout.root / "photobridge.db";
    layout.manifests = layout.root / "manifests";
    layout.plans = layout.root / "plans";
    layout.reports = layout.root / "reports";
    layout.logs = layout.root / "logs";
    layout.locks = layout.root / "locks";

    return layout;
}

}  // namespace photobridge
