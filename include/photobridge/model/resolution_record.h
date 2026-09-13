#pragma once

#include <optional>
#include <string>
#include <vector>

#include "photobridge/common/status.h"
#include "photobridge/model/metadata_candidate.h"

namespace photobridge {

struct ResolutionRecord {
    MetadataField field = MetadataField::kTitle;
    std::optional<MetadataValue> selected_value;
    std::optional<MetadataSource> selected_source;
    RuleId rule_id;
    std::string rule_version;
    std::vector<MetadataCandidate> candidates;
    bool conflict = false;
    std::string reason;
};

Status ValidateResolutionRecord(const ResolutionRecord& record) noexcept;

}  // namespace photobridge
