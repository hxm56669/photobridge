#pragma once

#include <vector>

#include "photobridge/common/status_or.h"
#include "photobridge/model/resolution_record.h"

namespace photobridge {

enum class ProvenanceOutcome {
    kSelected,
    kRejected,
};

struct ProvenanceRecord {
    MetadataField field = MetadataField::kTitle;
    PhysicalAssetId asset_id;
    MetadataValue value;
    MetadataSource source = MetadataSource::kGoogleTakeoutJson;
    RuleId rule_id;
    std::string rule_version;
    std::string evidence;
    ProvenanceOutcome outcome = ProvenanceOutcome::kRejected;
    std::string reason;
};

StatusOr<std::vector<ProvenanceRecord>> BuildProvenance(
    const ResolutionRecord& resolution);

}  // namespace photobridge
