#include <gtest/gtest.h>

#include "photobridge/model/metadata_resolver.h"

namespace {

photobridge::MetadataCandidate Candidate(
    std::string asset_id,
    std::string value,
    std::string evidence = "photo.json#/title")
{
    return photobridge::MetadataCandidate{
        std::move(asset_id),
        photobridge::MetadataField::kTitle,
        std::move(value),
        photobridge::MetadataSource::kGoogleTakeoutJson,
        "takeout.json.title.v1",
        std::move(evidence),
    };
}

}  // namespace

TEST(MetadataResolverTest, SelectsDeterministicallyAndRetainsAllCandidates)
{
    const auto& ruleset = photobridge::GoogleTakeoutMetadataRuleset();
    const std::vector<photobridge::MetadataCandidate> input{
        Candidate("asset-b", "same", "b"),
        Candidate("asset-a", "same", "a"),
    };
    photobridge::MetadataResolver resolver;
    const auto result = resolver.Resolve(
        photobridge::MetadataField::kTitle,
        input,
        ruleset);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(std::get<std::string>(*result.value().selected_value), "same");
    EXPECT_EQ(result.value().candidates[0].asset_id, "asset-a");
    EXPECT_FALSE(result.value().conflict);
    EXPECT_TRUE(photobridge::ValidateResolutionRecord(result.value()).ok());
}

TEST(MetadataResolverTest, RecordsConflictAndCanonicalTieBreak)
{
    const auto& ruleset = photobridge::GoogleTakeoutMetadataRuleset();
    const std::vector<photobridge::MetadataCandidate> input{
        Candidate("asset-b", "B"),
        Candidate("asset-a", "A"),
    };
    photobridge::MetadataResolver resolver;
    const auto result = resolver.Resolve(
        photobridge::MetadataField::kTitle,
        input,
        ruleset);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(std::get<std::string>(*result.value().selected_value), "A");
    EXPECT_TRUE(result.value().conflict);
    EXPECT_NE(result.value().reason.find("tie-break"), std::string::npos);
}

TEST(MetadataResolverTest, RejectsWrongFieldOrRule)
{
    auto candidate = Candidate("asset-a", "A");
    candidate.field = photobridge::MetadataField::kFavorite;
    photobridge::MetadataResolver resolver;
    const auto result = resolver.Resolve(
        photobridge::MetadataField::kTitle,
        std::vector<photobridge::MetadataCandidate>{candidate},
        photobridge::GoogleTakeoutMetadataRuleset());
    EXPECT_EQ(result.status().code(), photobridge::StatusCode::kInvalidArgument);
}

TEST(MetadataResolverTest, ReversingInputsDoesNotChangeResolution)
{
    const auto& ruleset = photobridge::GoogleTakeoutMetadataRuleset();
    const std::vector<photobridge::MetadataCandidate> first{
        Candidate("asset-c", "C", "c"),
        Candidate("asset-a", "A", "a"),
        Candidate("asset-b", "B", "b"),
    };
    const std::vector<photobridge::MetadataCandidate> second{
        first[2], first[0], first[1],
    };
    photobridge::MetadataResolver resolver;
    const auto left = resolver.Resolve(
        photobridge::MetadataField::kTitle, first, ruleset);
    const auto right = resolver.Resolve(
        photobridge::MetadataField::kTitle, second, ruleset);
    ASSERT_TRUE(left.ok());
    ASSERT_TRUE(right.ok());
    EXPECT_EQ(left.value().rule_version, right.value().rule_version);
    EXPECT_EQ(left.value().rule_id, right.value().rule_id);
    EXPECT_EQ(left.value().reason, right.value().reason);
    EXPECT_EQ(
        std::get<std::string>(*left.value().selected_value),
        std::get<std::string>(*right.value().selected_value));
    ASSERT_EQ(left.value().candidates.size(), right.value().candidates.size());
    for (std::size_t index = 0; index < left.value().candidates.size(); ++index) {
        EXPECT_EQ(
            left.value().candidates[index].asset_id,
            right.value().candidates[index].asset_id);
    }
}

TEST(MetadataResolverTest, BindsResultToRulesetVersion)
{
    auto ruleset = photobridge::GoogleTakeoutMetadataRuleset();
    photobridge::MetadataResolver resolver;
    const auto result = resolver.Resolve(
        photobridge::MetadataField::kTitle,
        std::vector<photobridge::MetadataCandidate>{Candidate("asset-a", "A")},
        ruleset);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().rule_version, "takeout-metadata-v1");
    ruleset.version = "takeout-metadata-v2";
    const auto upgraded = resolver.Resolve(
        photobridge::MetadataField::kTitle,
        std::vector<photobridge::MetadataCandidate>{Candidate("asset-a", "A")},
        ruleset);
    ASSERT_TRUE(upgraded.ok());
    EXPECT_EQ(upgraded.value().rule_version, "takeout-metadata-v2");
}
