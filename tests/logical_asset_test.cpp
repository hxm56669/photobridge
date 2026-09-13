#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "photobridge/model/logical_asset.h"

namespace {

photobridge::PhysicalAsset Asset(std::string path)
{
    auto relative_path = photobridge::RelativePath::Parse(std::move(path));
    EXPECT_TRUE(relative_path.ok());
    return photobridge::PhysicalAsset{
        std::move(relative_path.value()),
        photobridge::FileIdentity{},
        photobridge::AssetKind::kMedia,
        "jpg",
    };
}

}  // namespace

TEST(LogicalAssetTest, MapsOnePhysicalAssetToOneLogicalAsset)
{
    const auto physical = Asset("album/photo.jpg");
    const auto result =
        photobridge::MapPhysicalAssetToLogicalAsset(physical);

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().members.size(), 1U);
    EXPECT_EQ(
        result.value().members.front(),
        photobridge::PhysicalAssetIdFor(physical));
    EXPECT_EQ(
        result.value().id,
        photobridge::LogicalAssetIdForMembers(result.value().members).value());
}

TEST(LogicalAssetTest, PhysicalIdIsStableAndMetadataIndependent)
{
    auto first = Asset("album/photo.jpg");
    auto second = Asset("album/photo.jpg");
    second.identity.inode = 99;
    second.identity.size = 1234;
    second.kind = photobridge::AssetKind::kUnknown;

    EXPECT_EQ(
        photobridge::PhysicalAssetIdFor(first),
        photobridge::PhysicalAssetIdFor(second));
}

TEST(LogicalAssetTest, MemberOrderDoesNotChangeLogicalId)
{
    const auto first = photobridge::LogicalAssetIdForMembers(
        {"path-62", "path-61"});
    const auto second = photobridge::LogicalAssetIdForMembers(
        {"path-61", "path-62"});

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(first.value(), second.value());
}

TEST(LogicalAssetTest, DifferentMembersProduceDifferentLogicalIds)
{
    const auto first = photobridge::LogicalAssetIdForMembers({"path-61"});
    const auto second = photobridge::LogicalAssetIdForMembers({"path-62"});

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_NE(first.value(), second.value());
}

TEST(LogicalAssetTest, RejectsEmptyAndDuplicateMembers)
{
    const auto empty = photobridge::LogicalAssetIdForMembers({});
    const auto blank = photobridge::LogicalAssetIdForMembers({""});
    const auto duplicate = photobridge::LogicalAssetIdForMembers(
        {"path-61", "path-61"});

    EXPECT_FALSE(empty.ok());
    EXPECT_FALSE(blank.ok());
    EXPECT_FALSE(duplicate.ok());
}
