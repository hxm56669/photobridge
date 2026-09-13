#include "photobridge/model/photo_ir.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace photobridge {
namespace {

bool RelationshipLess(
    const PhotoRelationship& left,
    const PhotoRelationship& right)
{
    if (left.lhs != right.lhs) return left.lhs < right.lhs;
    if (left.rhs != right.rhs) return left.rhs < right.rhs;
    if (left.relation != right.relation) {
        return static_cast<int>(left.relation)
            < static_cast<int>(right.relation);
    }
    if (left.rule != right.rule) return left.rule < right.rule;
    if (left.evidence.rule != right.evidence.rule) {
        return left.evidence.rule < right.evidence.rule;
    }
    return left.evidence.detail < right.evidence.detail;
}

}  // namespace

StatusOr<CanonicalPhoto> BuildCanonicalPhoto(
    const LogicalAsset& logical_asset,
    std::span<const PhysicalAsset> physical_assets,
    std::span<const AssociationEdge> edges)
{
    if (logical_asset.id.empty() || logical_asset.members.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "canonical photo requires a non-empty logical asset");
    }

    auto expected_id = LogicalAssetIdForMembers(logical_asset.members);
    if (!expected_id.ok()) return expected_id.status();
    if (expected_id.value() != logical_asset.id) {
        return Status(
            StatusCode::kInvalidArgument,
            "canonical photo logical asset id does not match members");
    }

    std::unordered_map<PhysicalAssetId, const PhysicalAsset*> by_id;
    by_id.reserve(physical_assets.size());
    for (const PhysicalAsset& asset : physical_assets) {
        const PhysicalAssetId id = PhysicalAssetIdFor(asset);
        if (!by_id.emplace(id, &asset).second) {
            return Status(
                StatusCode::kInvalidArgument,
                "canonical photo physical asset ids must be unique");
        }
    }

    std::unordered_map<PhysicalAssetId, bool> member_set;
    member_set.reserve(logical_asset.members.size());
    for (const PhysicalAssetId& member : logical_asset.members) {
        if (!member_set.emplace(member, true).second) {
            return Status(
                StatusCode::kInvalidArgument,
                "canonical photo logical asset members must be unique");
        }
        if (by_id.find(member) == by_id.end()) {
            return Status(
                StatusCode::kNotFound,
                "canonical photo logical asset member is missing");
        }
    }

    CanonicalPhoto result;
    result.id = logical_asset.id;
    result.members = logical_asset.members;
    std::sort(result.members.begin(), result.members.end());

    for (const PhysicalAssetId& member : result.members) {
        const PhysicalAsset& asset = *by_id.at(member);
        if (asset.kind != AssetKind::kMedia) continue;
        result.media_components.push_back(PhotoMediaComponent{
            member,
            asset.relative_path,
            asset.kind,
            asset.extension,
        });
    }
    std::sort(
        result.media_components.begin(),
        result.media_components.end(),
        [](const PhotoMediaComponent& left, const PhotoMediaComponent& right) {
            return left.asset_id < right.asset_id;
        });

    for (const AssociationEdge& edge : edges) {
        if (edge.status != AssociationStatus::kConfirmed) continue;
        if (member_set.find(edge.lhs) == member_set.end()
            || member_set.find(edge.rhs) == member_set.end()) {
            continue;
        }
        result.relationships.push_back(PhotoRelationship{
            edge.lhs,
            edge.rhs,
            edge.relation,
            edge.status,
            edge.rule,
            edge.evidence,
        });
    }
    std::sort(
        result.relationships.begin(),
        result.relationships.end(),
        RelationshipLess);
    return result;
}

}  // namespace photobridge
