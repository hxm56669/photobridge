#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "photobridge/model/metadata_verifier.h"

TEST(MetadataVerifierTest, DistinguishesMatchMismatchAndMissingData)
{
    const photobridge::MetadataValue expected = std::string("title");
    const auto identical = photobridge::VerifyMetadata(expected, expected);
    EXPECT_EQ(
        identical.status,
        photobridge::MetadataVerification::kIdentical);

    const photobridge::MetadataValue changed = std::string("changed");
    const auto mismatch = photobridge::VerifyMetadata(expected, changed);
    EXPECT_EQ(
        mismatch.status,
        photobridge::MetadataVerification::kMismatch);

    const auto missing = photobridge::VerifyMetadata(
        std::nullopt,
        expected);
    EXPECT_EQ(
        missing.status,
        photobridge::MetadataVerification::kUnverifiable);
}
