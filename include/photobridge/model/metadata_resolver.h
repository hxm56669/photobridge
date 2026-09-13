#pragma once

#include <span>

#include "photobridge/common/status_or.h"
#include "photobridge/model/metadata_rules.h"
#include "photobridge/model/resolution_record.h"

namespace photobridge {

class MetadataResolver final {
public:
    StatusOr<ResolutionRecord> Resolve(
        MetadataField field,
        std::span<const MetadataCandidate> candidates,
        const MetadataRuleset& ruleset) const;
};

}  // namespace photobridge
