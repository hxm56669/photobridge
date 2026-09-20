#include <filesystem>

#include <gtest/gtest.h>

#include "photobridge/app/workspace_layout.h"

TEST(WorkspaceLayoutTest, DerivesStablePathsFromRoot)
{
    const auto result = photobridge::WorkspaceLayout::FromRoot(
        std::filesystem::path("/var/lib/photobridge"));

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(
        result.value().database,
        std::filesystem::path("/var/lib/photobridge/photobridge.db"));
    EXPECT_EQ(
        result.value().manifests,
        std::filesystem::path("/var/lib/photobridge/manifests"));
    EXPECT_EQ(
        result.value().plans,
        std::filesystem::path("/var/lib/photobridge/plans"));
    EXPECT_EQ(
        result.value().reports,
        std::filesystem::path("/var/lib/photobridge/reports"));
    EXPECT_EQ(
        result.value().logs,
        std::filesystem::path("/var/lib/photobridge/logs"));
    EXPECT_EQ(
        result.value().locks,
        std::filesystem::path("/var/lib/photobridge/locks"));
}

TEST(WorkspaceLayoutTest, RejectsEmptyRoot)
{
    const auto result = photobridge::WorkspaceLayout::FromRoot(
        std::filesystem::path{});

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(
        result.status().code(),
        photobridge::StatusCode::kInvalidArgument);
}
