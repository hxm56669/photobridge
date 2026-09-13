#include "photobridge/model/provenance.h"

#include <algorithm>

namespace photobridge {

StatusOr<std::vector<ProvenanceRecord>> BuildProvenance(
    const ResolutionRecord& resolution)
{
    const Status validation = ValidateResolutionRecord(resolution);
    if (!validation.ok()) return validation;

    std::vector<MetadataCandidate> candidates = resolution.candidates;
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const MetadataCandidate& left, const MetadataCandidate& right) {
            if (left.asset_id != right.asset_id) return left.asset_id < right.asset_id;
            if (left.extraction_rule != right.extraction_rule) {
                return left.extraction_rule < right.extraction_rule;
            }
            return left.evidence < right.evidence;
        });

    bool selected_written = false;
    std::vector<ProvenanceRecord> result;
    result.reserve(candidates.size());
    for (const MetadataCandidate& candidate : candidates) {
        const bool is_selected = !selected_written
            && resolution.selected_value.has_value()
            && resolution.selected_source.has_value()
            && candidate.source == resolution.selected_source.value()
            && candidate.extraction_rule == resolution.rule_id
            && MetadataValuesEqual(candidate.value, resolution.selected_value.value());
        result.push_back(ProvenanceRecord{
            candidate.field,
            candidate.asset_id,
            candidate.value,
            candidate.source,
            candidate.extraction_rule,
            resolution.rule_version,
            candidate.evidence,
            is_selected
                ? ProvenanceOutcome::kSelected
                : ProvenanceOutcome::kRejected,
            is_selected
                ? "selected by canonical resolver order"
                : resolution.conflict
                    ? "rejected by conflict tie-break"
                    : "not selected",
        });
        selected_written = selected_written || is_selected;
    }
    return result;
}

}  // namespace photobridge
