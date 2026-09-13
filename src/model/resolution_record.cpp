#include "photobridge/model/resolution_record.h"

namespace photobridge {

Status ValidateResolutionRecord(const ResolutionRecord& record) noexcept
{
    if (record.selected_value.has_value()
        != record.selected_source.has_value()) {
        return Status(
            StatusCode::kInvalidArgument,
            "resolution selected value and source must be provided together");
    }
    if (record.selected_value.has_value()
        && !IsMetadataValueCompatible(record.field, *record.selected_value)) {
        return Status(
            StatusCode::kInvalidArgument,
            "resolution selected value is incompatible with its field");
    }
    if (record.selected_value.has_value()
        && (record.rule_id.empty() || record.rule_version.empty())) {
        return Status(
            StatusCode::kInvalidArgument,
            "resolution selection must identify its rule and version");
    }
    if (record.conflict && record.reason.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "conflicted resolution must include a reason");
    }
    for (const MetadataCandidate& candidate : record.candidates) {
        if (candidate.asset_id.empty()
            || candidate.field != record.field
            || candidate.extraction_rule.empty()
            || !IsMetadataValueCompatible(candidate.field, candidate.value)) {
            return Status(
                StatusCode::kInvalidArgument,
                "resolution candidate is invalid for the record");
        }
    }
    return Status::Ok();
}

}  // namespace photobridge
