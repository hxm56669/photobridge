#include "photobridge/common/digest.h"

#include <gtest/gtest.h>

TEST(DigestTest, RoundTripsLowercaseHex)
{
    const auto digest = photobridge::Digest::FromHex(
        "00112233445566778899aabbccddeeff"
        "102132435465768798a9bacbdcedfe0f");

    ASSERT_TRUE(digest.ok()) << digest.status().message();
    EXPECT_EQ(
        digest.value().ToHex(),
        "00112233445566778899aabbccddeeff"
        "102132435465768798a9bacbdcedfe0f");
}

TEST(DigestTest, AcceptsUppercaseHexAndNormalizesOutput)
{
    const auto digest = photobridge::Digest::FromHex(
        "AABBCCDDEEFF00112233445566778899"
        "AABBCCDDEEFF00112233445566778899");

    ASSERT_TRUE(digest.ok()) << digest.status().message();
    EXPECT_EQ(
        digest.value().ToHex(),
        "aabbccddeeff00112233445566778899"
        "aabbccddeeff00112233445566778899");
}

TEST(DigestTest, RejectsMalformedHex)
{
    EXPECT_FALSE(photobridge::Digest::FromHex("00").ok());
    EXPECT_FALSE(
        photobridge::Digest::FromHex(
            "00112233445566778899aabbccddeeff"
            "102132435465768798a9bacbdcedfe0g").ok());
}
