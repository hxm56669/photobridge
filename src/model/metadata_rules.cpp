#include "photobridge/model/metadata_rules.h"

#include <algorithm>

namespace photobridge {

const MetadataRuleset& GoogleTakeoutMetadataRuleset() noexcept
{
    static const MetadataRuleset ruleset{
        "takeout-metadata-v1",
        {
            {"takeout.json.title.v1", MetadataField::kTitle,
             MetadataSource::kGoogleTakeoutJson},
            {"takeout.json.description.v1", MetadataField::kDescription,
             MetadataSource::kGoogleTakeoutJson},
            {"takeout.json.favorited.v1", MetadataField::kFavorite,
             MetadataSource::kGoogleTakeoutJson},
            {"takeout.json.photoTakenTime.timestamp.v1",
             MetadataField::kTakenTime, MetadataSource::kGoogleTakeoutJson},
        },
    };
    return ruleset;
}

Status ValidateMetadataRuleset(const MetadataRuleset& ruleset) noexcept
{
    if (ruleset.version.empty() || ruleset.chain.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "metadata ruleset must have a version and at least one rule");
    }
    for (const MetadataRule& rule : ruleset.chain) {
        if (rule.id.empty()) {
            return Status(
                StatusCode::kInvalidArgument,
                "metadata rule id must not be empty");
        }
        if (std::count_if(
                ruleset.chain.begin(),
                ruleset.chain.end(),
                [&rule](const MetadataRule& other) {
                    return other.id == rule.id;
                }) != 1) {
            return Status(
                StatusCode::kInvalidArgument,
                "metadata rule ids must be unique");
        }
    }
    return Status::Ok();
}

bool IsMetadataRuleCompatible(
    const MetadataRuleset& ruleset,
    MetadataField field,
    MetadataSource source,
    const RuleId& rule_id) noexcept
{
    return std::any_of(
        ruleset.chain.begin(),
        ruleset.chain.end(),
        [field, source, &rule_id](const MetadataRule& rule) {
            return rule.id == rule_id
                && rule.field == field
                && rule.source == source;
        });
}

}  // namespace photobridge
