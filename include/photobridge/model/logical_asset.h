#pragma once

#include <optional>
#include <string>
#include <vector>

#include "photobridge/common/status_or.h"
#include "photobridge/model/physical_asset.h"

namespace photobridge {

using LogicalAssetId = std::string;

struct LogicalAsset {
    LogicalAssetId id;
    std::vector<PhysicalAssetId> members;
    // L0 carries the source path needed by the target path planner. Later
    // semantic models can replace this with an explicit media component.
    std::optional<RelativePath> source_path;
};

// Creates the deterministic L0 one-member logical asset. The returned
// LogicalAsset is a value object; it has no database or filesystem effects.
StatusOr<LogicalAsset> MapPhysicalAssetToLogicalAsset(
    const PhysicalAsset& asset);

// Generates an ID from the sorted canonical member list. This is public so
// later association code can reuse the exact L0 identity rule.
StatusOr<LogicalAssetId> LogicalAssetIdForMembers(
    std::vector<PhysicalAssetId> members);

}  // namespace photobridge
