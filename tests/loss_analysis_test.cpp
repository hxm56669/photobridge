#include <gtest/gtest.h>

#include "photobridge/model/loss_analysis.h"

TEST(LossAnalysisTest, ReportsNonFullCapabilitiesWithoutChangingContract)
{
    const auto capabilities = photobridge::LocalDirectoryCapabilitiesV1();
    const auto result = photobridge::AnalyzeCapabilityLoss(capabilities);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().capability_version, "directory-v1");
    ASSERT_EQ(result.value().losses.size(), 3U);
    EXPECT_EQ(
        result.value().losses[0].kind,
        photobridge::CapabilityKind::kSymlinks);
    EXPECT_EQ(
        result.value().losses[1].level,
        photobridge::SupportLevel::kUnverifiable);
    EXPECT_EQ(
        result.value().losses[2].kind,
        photobridge::CapabilityKind::kUnicodeNormalization);
}

TEST(LossAnalysisTest, RejectsInvalidCapabilities)
{
    auto capabilities = photobridge::LocalDirectoryCapabilitiesV1();
    capabilities.entries[0].representation.clear();
    const auto result = photobridge::AnalyzeCapabilityLoss(capabilities);
    EXPECT_EQ(result.status().code(), photobridge::StatusCode::kInvalidArgument);
}
