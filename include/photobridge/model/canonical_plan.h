#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "photobridge/common/digest.h"
#include "photobridge/common/status_or.h"
#include "photobridge/model/capability.h"
#include "photobridge/model/file_identity.h"
#include "photobridge/model/logical_asset.h"
#include "photobridge/model/path_mapper.h"

namespace photobridge {

inline constexpr std::uint32_t kCanonicalMinimalPlanVersion = 1;

struct MinimalPlanAsset {
    LogicalAssetId logical_asset_id;
    PhysicalAssetId source_asset_id;
    RelativePath source_path;
    RelativePath target_path;
    FileIdentity source_identity;
};

struct MinimalPlanInput {
    std::string source_manifest_id;
    Digest source_manifest_digest;
    std::string target_root;
    TargetCapabilities capabilities;
    MigrationPolicy policy;
    std::vector<MinimalPlanAsset> assets;
};

// Public planner boundary name. MinimalPlanInput remains an alias-compatible
// name for the L0 teaching subset and existing artifact readers.
using PlannerInput = MinimalPlanInput;

class CanonicalMinimalPlan final {
public:
    static StatusOr<CanonicalMinimalPlan> Build(PlannerInput input);

    std::uint32_t format_version() const noexcept;
    const std::string& source_manifest_id() const noexcept;
    const Digest& source_manifest_digest() const noexcept;
    const std::string& target_root() const noexcept;
    const TargetCapabilities& capabilities() const noexcept;
    const MigrationPolicy& policy() const noexcept;
    const std::vector<MinimalPlanAsset>& assets() const noexcept;

    // Returns the deterministic semantic projection for C5's digest and
    // storage adapters. It contains no random IDs, timestamps, or runtime
    // state, and uses length-prefixed binary fields.
    std::string CanonicalPlanPayload() const;

    // Compatibility name retained for the L0 teaching API.
    std::string CanonicalPayload() const;

private:
    explicit CanonicalMinimalPlan(MinimalPlanInput input);

    MinimalPlanInput input_;
};

}  // namespace photobridge
