#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "photobridge/common/status_or.h"
#include "photobridge/model/capability.h"
#include "photobridge/model/logical_asset.h"

namespace photobridge {

struct MigrationPolicy {
    // Empty means that the source-relative path is mapped at the target root.
    std::string target_prefix;
    bool preserve_source_directories = true;
    std::size_t max_component_bytes = 255;
    std::size_t max_path_bytes = 4096;

    Status Validate() const;
};

struct PathMapping {
    LogicalAssetId logical_asset_id;
    RelativePath target_path;
};

class TargetPathMapper final {
public:
    StatusOr<PathMapping> Map(
        const LogicalAsset& asset,
        const TargetCapabilities& capabilities,
        const MigrationPolicy& policy) const;

    // MapAll is the planning boundary where collisions between assets can be
    // rejected before any executor or filesystem operation runs.
    StatusOr<std::vector<PathMapping>> MapAll(
        const std::vector<LogicalAsset>& assets,
        const TargetCapabilities& capabilities,
        const MigrationPolicy& policy) const;
};

}  // namespace photobridge
