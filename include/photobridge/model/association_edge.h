#pragma once

#include <string>

#include "photobridge/model/source_relation_candidate.h"

namespace photobridge {

enum class EdgeKind {
    kComposition,
    kMetadataAttachment,
    kAssetRelation,
};

enum class RelationType {
    kJsonSidecar,
    kXmpSidecar,
    kLivePhotoMotion,
    kRawJpegPair,
};

enum class AssociationStatus {
    kConfirmed,
    kAmbiguous,
    kRejected,
};

struct Evidence {
    RuleId rule;
    std::string detail;
};

struct AssociationEdge {
    EdgeKind edge_kind = EdgeKind::kAssetRelation;
    PhysicalAssetId lhs;
    PhysicalAssetId rhs;
    RelationType relation = RelationType::kJsonSidecar;
    RuleId rule;
    Evidence evidence;
    AssociationStatus status = AssociationStatus::kAmbiguous;
};

AssociationEdge AssociationEdgeFor(
    const SourceRelationCandidate& candidate);

}  // namespace photobridge
