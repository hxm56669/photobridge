#include <gtest/gtest.h>

#include "photobridge/model/metadata_rules.h"

TEST(MetadataRulesTest, TakeoutRulesetHasStableVersionedOrder)
{
    const auto& ruleset = photobridge::GoogleTakeoutMetadataRuleset();
    EXPECT_TRUE(photobridge::ValidateMetadataRuleset(ruleset).ok());
    ASSERT_EQ(ruleset.version, "takeout-metadata-v1");
    ASSERT_EQ(ruleset.chain.size(), 4U);
    EXPECT_EQ(ruleset.chain[0].id, "takeout.json.title.v1");
    EXPECT_EQ(ruleset.chain[1].id, "takeout.json.description.v1");
    EXPECT_EQ(ruleset.chain[2].id, "takeout.json.favorited.v1");
    EXPECT_EQ(
        ruleset.chain[3].id,
        "takeout.json.photoTakenTime.timestamp.v1");
}

TEST(MetadataRulesTest, BindsRuleToFieldAndSource)
{
    const auto& ruleset = photobridge::GoogleTakeoutMetadataRuleset();
    EXPECT_TRUE(photobridge::IsMetadataRuleCompatible(
        ruleset,
        photobridge::MetadataField::kTitle,
        photobridge::MetadataSource::kGoogleTakeoutJson,
        "takeout.json.title.v1"));
    EXPECT_FALSE(photobridge::IsMetadataRuleCompatible(
        ruleset,
        photobridge::MetadataField::kFavorite,
        photobridge::MetadataSource::kGoogleTakeoutJson,
        "takeout.json.title.v1"));
}

TEST(MetadataRulesTest, RejectsDuplicateOrEmptyRuleIds)
{
    auto invalid = photobridge::GoogleTakeoutMetadataRuleset();
    invalid.chain.push_back(invalid.chain.front());
    EXPECT_EQ(
        photobridge::ValidateMetadataRuleset(invalid).code(),
        photobridge::StatusCode::kInvalidArgument);
    invalid = photobridge::MetadataRuleset{"v1", {{"", photobridge::MetadataField::kTitle, photobridge::MetadataSource::kGoogleTakeoutJson}}};
    EXPECT_EQ(
        photobridge::ValidateMetadataRuleset(invalid).code(),
        photobridge::StatusCode::kInvalidArgument);
}
