#pragma once

#include <string>
#include <vector>

#include "photobridge/common/status.h"
#include "photobridge/model/metadata_candidate.h"

namespace photobridge {

struct MetadataRule {
    RuleId id;
    MetadataField field = MetadataField::kTitle;
    MetadataSource source = MetadataSource::kGoogleTakeoutJson;
};

// The vector order is the explicit, versioned rule-chain order. It is not a
// winner selection order; all matching candidates remain candidates.
struct MetadataRuleset {
    std::string version;
    std::vector<MetadataRule> chain;
};

const MetadataRuleset& GoogleTakeoutMetadataRuleset() noexcept;

Status ValidateMetadataRuleset(const MetadataRuleset& ruleset) noexcept;

bool IsMetadataRuleCompatible(
    const MetadataRuleset& ruleset,
    MetadataField field,
    MetadataSource source,
    const RuleId& rule_id) noexcept;

}  // namespace photobridge
