#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/model/canonical_plan.h"

namespace {

photobridge::MinimalPlanAsset Asset(
    std::string logical_id,
    std::string source_id,
    std::string source_path,
    std::string target_path,
    std::uint64_t inode)
{
    auto source = photobridge::RelativePath::Parse(std::move(source_path));
    auto target = photobridge::RelativePath::Parse(std::move(target_path));
    EXPECT_TRUE(source.ok());
    EXPECT_TRUE(target.ok());

    photobridge::FileIdentity identity;
    identity.device = 7;
    identity.inode = inode;
    identity.size = 1234;
    identity.mtime_ns = 5678;
    identity.ctime_ns = 9012;
    return photobridge::MinimalPlanAsset{
        std::move(logical_id),
        std::move(source_id),
        std::move(source.value()),
        std::move(target.value()),
        identity,
    };
}

photobridge::MinimalPlanInput Input(
    std::vector<photobridge::MinimalPlanAsset> assets)
{
    photobridge::MinimalPlanInput input;
    input.source_manifest_id = "manifest-1";
    input.source_manifest_digest.bytes[0] = std::byte{0x42};
    input.target_root = "/target/root";
    input.capabilities = photobridge::LocalDirectoryCapabilitiesV1();
    input.assets = std::move(assets);
    return input;
}

}  // namespace

TEST(CanonicalPlanTest, SortsAssetsAndProducesStablePayload)
{
    auto first_input = Input({
        Asset("logical-z", "physical-z", "z/photo.jpg", "z/photo.jpg", 2),
        Asset("logical-a", "physical-a", "a/photo.jpg", "a/photo.jpg", 1),
    });
    auto second_input = Input({
        Asset("logical-a", "physical-a", "a/photo.jpg", "a/photo.jpg", 1),
        Asset("logical-z", "physical-z", "z/photo.jpg", "z/photo.jpg", 2),
    });

    const auto first = photobridge::CanonicalMinimalPlan::Build(
        std::move(first_input));
    const auto second = photobridge::CanonicalMinimalPlan::Build(
        std::move(second_input));

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    ASSERT_EQ(first.value().assets().size(), 2U);
    EXPECT_EQ(first.value().assets()[0].logical_asset_id, "logical-a");
    EXPECT_EQ(first.value().assets()[1].logical_asset_id, "logical-z");
    EXPECT_EQ(
        first.value().CanonicalPayload(),
        second.value().CanonicalPayload());
    EXPECT_EQ(
        first.value().CanonicalPayload(),
        first.value().CanonicalPlanPayload());
}

TEST(CanonicalPlanTest, PayloadBindsManifestTargetAndPolicyInputs)
{
    const auto base = photobridge::CanonicalMinimalPlan::Build(
        Input({Asset(
            "logical-a",
            "physical-a",
            "a/photo.jpg",
            "a/photo.jpg",
            1)}));
    ASSERT_TRUE(base.ok());

    auto changed = Input({Asset(
        "logical-a",
        "physical-a",
        "a/photo.jpg",
        "a/photo.jpg",
        1)});
    changed.target_root = "/other/root";
    changed.policy.target_prefix = "assets";
    changed.source_manifest_digest.bytes[0] = std::byte{0x43};
    const auto result = photobridge::CanonicalMinimalPlan::Build(
        std::move(changed));

    ASSERT_TRUE(result.ok());
    EXPECT_NE(
        base.value().CanonicalPayload(),
        result.value().CanonicalPayload());
}

TEST(CanonicalPlanTest, RejectsDuplicateIdsAndMissingBindings)
{
    auto duplicate = Input({
        Asset("logical-a", "physical-a", "a.jpg", "a.jpg", 1),
        Asset("logical-a", "physical-b", "b.jpg", "b.jpg", 2),
    });
    auto missing_manifest = Input({});
    missing_manifest.source_manifest_id.clear();
    auto missing_root = Input({});
    missing_root.target_root.clear();

    const auto duplicate_result = photobridge::CanonicalMinimalPlan::Build(
        std::move(duplicate));
    const auto manifest_result = photobridge::CanonicalMinimalPlan::Build(
        std::move(missing_manifest));
    const auto root_result = photobridge::CanonicalMinimalPlan::Build(
        std::move(missing_root));

    EXPECT_FALSE(duplicate_result.ok());
    EXPECT_FALSE(manifest_result.ok());
    EXPECT_FALSE(root_result.ok());
}

TEST(CanonicalPlanTest, KeepsFrozenInputsReadOnly)
{
    const auto result = photobridge::CanonicalMinimalPlan::Build(
        Input({Asset(
            "logical-a",
            "physical-a",
            "a/photo.jpg",
            "a/photo.jpg",
            1)}));

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().format_version(), 1U);
    EXPECT_EQ(result.value().source_manifest_id(), "manifest-1");
    EXPECT_EQ(result.value().target_root(), "/target/root");
}
