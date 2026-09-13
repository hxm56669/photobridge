#pragma once

#include <string>

#include "photobridge/model/metadata_candidate.h"

namespace photobridge {

enum class SourceRelationKind {
    kJsonSidecarForMedia,
    kXmpSidecarForMedia,
    kLivePhotoMotionForMedia,
    kRawJpegPair,
};

struct SourceRelationCandidate {
    PhysicalAssetId media_asset_id;
    PhysicalAssetId sidecar_asset_id;
    SourceRelationKind relation = SourceRelationKind::kJsonSidecarForMedia;
    RuleId rule;
    std::string evidence;
};

}  // namespace photobridge
