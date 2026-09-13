#include <gtest/gtest.h>

#include "photobridge/model/resolution_record.h"

namespace {

photobridge::MetadataCandidate Candidate()
{
    return photobridge::MetadataCandidate{
        "asset-1",
        photobridge::MetadataField::kTitle,
        std::string("A photo"),
        photobridge::MetadataSource::kGoogleTakeoutJson,
        "takeout.json.title.v1",
        "photo.json#/title",
    };
}

photobridge::ResolutionRecord ValidRecord()
{
    return photobridge::ResolutionRecord{
        photobridge::MetadataField::kTitle,
        std::string("A photo"),
        photobridge::MetadataSource::kGoogleTakeoutJson,
        "takeout.json.title.v1",
        "takeout-metadata-v1",
        {Candidate()},
        false,
        "selected by the future resolver",
    };
}

}  // namespace

TEST(ResolutionRecordTest, CarriesSelectionCandidatesAndRuleProvenance)
{
    const auto record = ValidRecord();
    EXPECT_TRUE(photobridge::ValidateResolutionRecord(record).ok());
    ASSERT_TRUE(record.selected_value.has_value());
    EXPECT_EQ(std::get<std::string>(*record.selected_value), "A photo");
    ASSERT_EQ(record.candidates.size(), 1U);
    EXPECT_EQ(record.candidates[0].evidence, "photo.json#/title");
    EXPECT_EQ(record.rule_version, "takeout-metadata-v1");
}

TEST(ResolutionRecordTest, RequiresPairedSelectionAndRuleIdentity)
{
    auto record = ValidRecord();
    record.selected_source.reset();
    EXPECT_EQ(
        photobridge::ValidateResolutionRecord(record).code(),
        photobridge::StatusCode::kInvalidArgument);
    record = ValidRecord();
    record.rule_id.clear();
    EXPECT_EQ(
        photobridge::ValidateResolutionRecord(record).code(),
        photobridge::StatusCode::kInvalidArgument);
}

TEST(ResolutionRecordTest, ConflictRequiresReasonAndMatchingCandidates)
{
    auto record = ValidRecord();
    record.conflict = true;
    record.reason.clear();
    EXPECT_EQ(
        photobridge::ValidateResolutionRecord(record).code(),
        photobridge::StatusCode::kInvalidArgument);
    record = ValidRecord();
    record.candidates[0].field = photobridge::MetadataField::kFavorite;
    EXPECT_EQ(
        photobridge::ValidateResolutionRecord(record).code(),
        photobridge::StatusCode::kInvalidArgument);
}
