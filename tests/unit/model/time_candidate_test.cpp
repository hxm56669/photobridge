#include <gtest/gtest.h>

#include "photobridge/model/metadata_candidate.h"

namespace {

photobridge::TimeCandidate Absolute()
{
    return photobridge::TimeCandidate{
        photobridge::AbsoluteTime{1234567890000000000LL},
        480,
        photobridge::TimePrecision::kSecond,
        photobridge::MetadataSource::kGoogleTakeoutJson,
        "1234567890",
    };
}

}  // namespace

TEST(TimeCandidateTest, AcceptsAbsoluteTimeWithoutReinterpretingIt)
{
    const auto candidate = Absolute();
    EXPECT_TRUE(photobridge::ValidateTimeCandidate(candidate).ok());
    EXPECT_EQ(
        std::get<photobridge::AbsoluteTime>(candidate.value).unix_ns,
        1234567890000000000LL);
    EXPECT_EQ(candidate.utc_offset_minutes.value(), 480);
}

TEST(TimeCandidateTest, ValidatesLocalCalendarAndOffsetStructure)
{
    auto candidate = Absolute();
    candidate.value = photobridge::LocalDateTime{2024, 2, 29, 23, 59, 59, 0};
    EXPECT_TRUE(photobridge::ValidateTimeCandidate(candidate).ok());

    candidate.value = photobridge::LocalDateTime{2024, 13, 1, 0, 0, 0, 0};
    EXPECT_EQ(
        photobridge::ValidateTimeCandidate(candidate).code(),
        photobridge::StatusCode::kInvalidArgument);
    candidate = Absolute();
    candidate.utc_offset_minutes = 15 * 60;
    EXPECT_EQ(
        photobridge::ValidateTimeCandidate(candidate).code(),
        photobridge::StatusCode::kInvalidArgument);
}

TEST(TimeCandidateTest, RequiresRawValueAndKnownEnumValues)
{
    auto candidate = Absolute();
    candidate.raw_value.clear();
    EXPECT_EQ(
        photobridge::ValidateTimeCandidate(candidate).code(),
        photobridge::StatusCode::kInvalidArgument);
    candidate = Absolute();
    candidate.precision = static_cast<photobridge::TimePrecision>(99);
    EXPECT_EQ(
        photobridge::ValidateTimeCandidate(candidate).code(),
        photobridge::StatusCode::kInvalidArgument);
}
