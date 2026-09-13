#include "photobridge/model/metadata_resolver.h"

#include <algorithm>
#include <cstddef>

namespace photobridge {
namespace {

std::size_t RuleRank(
    const MetadataRuleset& ruleset,
    const RuleId& rule_id)
{
    for (std::size_t index = 0; index < ruleset.chain.size(); ++index) {
        if (ruleset.chain[index].id == rule_id) return index;
    }
    return ruleset.chain.size();
}

bool CandidateLess(
    const MetadataCandidate& left,
    const MetadataCandidate& right,
    const MetadataRuleset& ruleset)
{
    const std::size_t left_rank = RuleRank(ruleset, left.extraction_rule);
    const std::size_t right_rank = RuleRank(ruleset, right.extraction_rule);
    if (left_rank != right_rank) return left_rank < right_rank;
    if (left.asset_id != right.asset_id) return left.asset_id < right.asset_id;
    if (left.extraction_rule != right.extraction_rule) {
        return left.extraction_rule < right.extraction_rule;
    }
    return left.evidence < right.evidence;
}

}  // namespace

StatusOr<ResolutionRecord> MetadataResolver::Resolve(
    MetadataField field,
    std::span<const MetadataCandidate> candidates,
    const MetadataRuleset& ruleset) const
{
    const Status ruleset_status = ValidateMetadataRuleset(ruleset);
    if (!ruleset_status.ok()) return ruleset_status;

    ResolutionRecord result;
    result.field = field;
    result.rule_version = ruleset.version;
    result.candidates.assign(candidates.begin(), candidates.end());
    for (const MetadataCandidate& candidate : result.candidates) {
        if (candidate.field != field
            || candidate.asset_id.empty()
            || !IsMetadataRuleCompatible(
                ruleset,
                candidate.field,
                candidate.source,
                candidate.extraction_rule)
            || !IsMetadataValueCompatible(candidate.field, candidate.value)) {
            return Status(
                StatusCode::kInvalidArgument,
                "metadata candidate is not valid for the requested ruleset");
        }
    }
    std::sort(
        result.candidates.begin(),
        result.candidates.end(),
        [&ruleset](const MetadataCandidate& left, const MetadataCandidate& right) {
            return CandidateLess(left, right, ruleset);
        });
    if (result.candidates.empty()) {
        result.reason = "no candidates";
        return result;
    }

    const MetadataCandidate& selected = result.candidates.front();
    result.selected_value = selected.value;
    result.selected_source = selected.source;
    result.rule_id = selected.extraction_rule;
    for (const MetadataCandidate& candidate : result.candidates) {
        if (!MetadataValuesEqual(candidate.value, selected.value)) {
            result.conflict = true;
            result.reason = "multiple distinct values; canonical tie-break selected the first candidate";
            break;
        }
    }
    if (!result.conflict) result.reason = "single canonical value";
    return result;
}

}  // namespace photobridge
