#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "photobridge/model/metadata_candidate.h"

TEST(MetadataCandidateTest, ReportsTheControlledVariantKind)
{
    const photobridge::MetadataValue string_value = std::string("title");
    const photobridge::MetadataValue integer_value = std::int64_t{42};
    const photobridge::MetadataValue boolean_value = true;
    const photobridge::MetadataValue time_value = photobridge::TimeCandidate{
        photobridge::AbsoluteTime{123},
        std::nullopt,
        photobridge::TimePrecision::kSecond,
        photobridge::MetadataSource::kGoogleTakeoutJson,
        "123",
    };

    EXPECT_EQ(
        photobridge::MetadataValueKindOf(string_value),
        photobridge::MetadataValueKind::kString);
    EXPECT_EQ(
        photobridge::MetadataValueKindOf(integer_value),
        photobridge::MetadataValueKind::kInt64);
    EXPECT_EQ(
        photobridge::MetadataValueKindOf(boolean_value),
        photobridge::MetadataValueKind::kBool);
    EXPECT_EQ(
        photobridge::MetadataValueKindOf(time_value),
        photobridge::MetadataValueKind::kTimeCandidate);
}

TEST(MetadataCandidateTest, EnforcesTheInitialFieldTypeContract)
{
    const photobridge::MetadataValue title = std::string("title");
    const photobridge::MetadataValue description = std::string("description");
    const photobridge::MetadataValue favorite = true;
    const photobridge::MetadataValue time = photobridge::TimeCandidate{
        photobridge::AbsoluteTime{123},
        std::nullopt,
        photobridge::TimePrecision::kSecond,
        photobridge::MetadataSource::kGoogleTakeoutJson,
        "123",
    };

    EXPECT_TRUE(photobridge::IsMetadataValueCompatible(
        photobridge::MetadataField::kTitle, title));
    EXPECT_TRUE(photobridge::IsMetadataValueCompatible(
        photobridge::MetadataField::kDescription, description));
    EXPECT_TRUE(photobridge::IsMetadataValueCompatible(
        photobridge::MetadataField::kFavorite, favorite));
    EXPECT_TRUE(photobridge::IsMetadataValueCompatible(
        photobridge::MetadataField::kTakenTime, time));
    EXPECT_FALSE(photobridge::IsMetadataValueCompatible(
        photobridge::MetadataField::kTitle, favorite));
    EXPECT_FALSE(photobridge::IsMetadataValueCompatible(
        photobridge::MetadataField::kTakenTime, title));
}
