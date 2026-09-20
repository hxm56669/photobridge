#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/model/logical_asset.h"
#include "photobridge/model/path_mapper.h"

namespace {

photobridge::LogicalAsset Asset(std::string path, std::string id)
{
    auto source_path = photobridge::RelativePath::Parse(std::move(path));
    EXPECT_TRUE(source_path.ok());
    return photobridge::LogicalAsset{
        std::move(id),
        {"physical-" + source_path.value().DisplayString()},
        std::move(source_path.value()),
    };
}

photobridge::MigrationPolicy Policy()
{
    return photobridge::MigrationPolicy{
        "",
        true,
        255,
        4096,
    };
}

}  // namespace

TEST(PathMapperTest, PreservesRelativePathAndExtension)
{
    const auto capabilities =
        photobridge::LocalDirectoryCapabilitiesV1();
    const auto asset = Asset("album/photo.JPG", "logical-1");

    const auto result = photobridge::TargetPathMapper().Map(
        asset,
        capabilities,
        Policy());

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().logical_asset_id, "logical-1");
    EXPECT_EQ(result.value().target_path.bytes(), "album/photo.JPG");
}

TEST(PathMapperTest, SupportsExplicitPrefixAndFlattening)
{
    const auto capabilities =
        photobridge::LocalDirectoryCapabilitiesV1();
    const auto asset = Asset("album/photo.JPG", "logical-1");
    auto policy = Policy();
    policy.target_prefix = "assets";
    policy.preserve_source_directories = false;

    const auto result = photobridge::TargetPathMapper().Map(
        asset,
        capabilities,
        policy);

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().target_path.bytes(), "assets/photo.JPG");
}

TEST(PathMapperTest, RejectsExactAndCaseFoldCollisions)
{
    const auto capabilities =
        photobridge::LocalDirectoryCapabilitiesV1();
    const std::vector<photobridge::LogicalAsset> exact{
        Asset("one/photo.jpg", "logical-1"),
        Asset("two/photo.jpg", "logical-2"),
    };
    const std::vector<photobridge::LogicalAsset> folded{
        Asset("one/Photo.jpg", "logical-1"),
        Asset("two/photo.JPG", "logical-2"),
    };
    auto policy = Policy();
    policy.preserve_source_directories = false;

    const auto exact_result = photobridge::TargetPathMapper().MapAll(
        exact,
        capabilities,
        policy);
    const auto folded_result = photobridge::TargetPathMapper().MapAll(
        folded,
        capabilities,
        policy);

    ASSERT_FALSE(exact_result.ok());
    EXPECT_EQ(
        exact_result.status().code(),
        photobridge::StatusCode::kAlreadyExists);
    ASSERT_FALSE(folded_result.ok());
    EXPECT_EQ(
        folded_result.status().code(),
        photobridge::StatusCode::kAlreadyExists);
}

TEST(PathMapperTest, SortsSuccessfulBatchByLogicalAssetId)
{
    const auto capabilities =
        photobridge::LocalDirectoryCapabilitiesV1();
    const std::vector<photobridge::LogicalAsset> assets{
        Asset("z/photo.jpg", "logical-z"),
        Asset("a/photo.jpg", "logical-a"),
    };

    const auto result = photobridge::TargetPathMapper().MapAll(
        assets,
        capabilities,
        Policy());

    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value().size(), 2U);
    EXPECT_EQ(result.value()[0].logical_asset_id, "logical-a");
    EXPECT_EQ(result.value()[1].logical_asset_id, "logical-z");
}

TEST(PathMapperTest, RejectsUnsafeOrOversizedPaths)
{
    const auto capabilities =
        photobridge::LocalDirectoryCapabilitiesV1();
    const auto asset = Asset("album/photo.jpg", "logical-1");
    auto invalid_prefix = Policy();
    invalid_prefix.target_prefix = "../target";
    auto short_limit = Policy();
    short_limit.max_component_bytes = 4;

    const auto invalid_result = photobridge::TargetPathMapper().Map(
        asset,
        capabilities,
        invalid_prefix);
    const auto oversized_result = photobridge::TargetPathMapper().Map(
        asset,
        capabilities,
        short_limit);

    EXPECT_FALSE(invalid_result.ok());
    EXPECT_EQ(
        invalid_result.status().code(),
        photobridge::StatusCode::kInvalidArgument);
    EXPECT_FALSE(oversized_result.ok());
    EXPECT_EQ(
        oversized_result.status().code(),
        photobridge::StatusCode::kInvalidArgument);
}

TEST(PathMapperTest, RejectsMissingSourcePathAndUnverifiableUnicode)
{
    const auto capabilities =
        photobridge::LocalDirectoryCapabilitiesV1();
    const photobridge::LogicalAsset missing{
        "logical-1",
        {"physical-1"},
        std::nullopt,
    };
    const auto unicode = Asset("album/照片.jpg", "logical-2");

    const auto missing_result = photobridge::TargetPathMapper().Map(
        missing,
        capabilities,
        Policy());
    const auto unicode_result = photobridge::TargetPathMapper().Map(
        unicode,
        capabilities,
        Policy());

    EXPECT_FALSE(missing_result.ok());
    EXPECT_FALSE(unicode_result.ok());
}
