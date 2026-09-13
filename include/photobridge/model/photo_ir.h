#pragma once

#include <span>
#include <string>
#include <vector>

#include "photobridge/common/status_or.h"
#include "photobridge/model/association_edge.h"
#include "photobridge/model/logical_asset.h"

namespace photobridge {

// The first semantic projection after association.  A component is still
// backed by its physical asset; metadata resolution is deliberately outside
// this value object.
struct PhotoMediaComponent {
    PhysicalAssetId asset_id;
    RelativePath relative_path;
    AssetKind kind = AssetKind::kUnknown;
    std::string extension;
};

struct PhotoRelationship {
    PhysicalAssetId lhs;
    PhysicalAssetId rhs;
    RelationType relation = RelationType::kJsonSidecar;
    AssociationStatus status = AssociationStatus::kConfirmed;
    RuleId rule;
    Evidence evidence;
};

struct CanonicalPhoto {
    LogicalAssetId id;
    std::vector<PhysicalAssetId> members;
    std::vector<PhotoMediaComponent> media_components;
    std::vector<PhotoRelationship> relationships;
};

// Projects one already-associated LogicalAsset into the minimal canonical
// photo IR.  Only confirmed edges are emitted as relationships; their rule
// and evidence remain available for later provenance and conflict handling.
StatusOr<CanonicalPhoto> BuildCanonicalPhoto(
    const LogicalAsset& logical_asset,
    std::span<const PhysicalAsset> physical_assets,
    std::span<const AssociationEdge> edges);

}  // namespace photobridge
