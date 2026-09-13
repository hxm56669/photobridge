#include "photobridge/model/association_edge.h"

namespace photobridge {

AssociationEdge AssociationEdgeFor(
    const SourceRelationCandidate& candidate)
{
    return AssociationEdge{
        candidate.relation == SourceRelationKind::kLivePhotoMotionForMedia
                || candidate.relation == SourceRelationKind::kRawJpegPair
            ? EdgeKind::kComposition
            : EdgeKind::kMetadataAttachment,
        candidate.media_asset_id,
        candidate.sidecar_asset_id,
        candidate.relation == SourceRelationKind::kLivePhotoMotionForMedia
            ? RelationType::kLivePhotoMotion
            : (candidate.relation == SourceRelationKind::kRawJpegPair
                ? RelationType::kRawJpegPair
                : (candidate.relation == SourceRelationKind::kXmpSidecarForMedia
                    ? RelationType::kXmpSidecar
                    : RelationType::kJsonSidecar)),
        candidate.rule,
        Evidence{candidate.rule, candidate.evidence},
        AssociationStatus::kConfirmed,
    };
}

}  // namespace photobridge
