#include <cstddef>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "photobridge/model/plan_artifact.h"

namespace {

photobridge::MinimalPlanAsset Asset(
    std::string logical_id,
    std::string source_id,
    std::string source_path,
    std::string target_path)
{
    auto source = photobridge::RelativePath::Parse(std::move(source_path));
    auto target = photobridge::RelativePath::Parse(std::move(target_path));
    EXPECT_TRUE(source.ok());
    EXPECT_TRUE(target.ok());

    photobridge::FileIdentity identity;
    identity.device = 7;
    identity.inode = 42;
    identity.size = 1234;
    identity.mtime_ns = -5678;
    identity.ctime_ns = 9012;
    identity.mount_id = 11;
    return photobridge::MinimalPlanAsset{
        std::move(logical_id),
        std::move(source_id),
        std::move(source.value()),
        std::move(target.value()),
        identity,
    };
}

photobridge::CanonicalMinimalPlan MakePlan()
{
    photobridge::MinimalPlanInput input;
    input.source_manifest_id = "manifest-1";
    input.source_manifest_digest.bytes[0] = std::byte{0x42};
    input.target_root = "/target/root";
    input.capabilities = photobridge::LocalDirectoryCapabilitiesV1();
    input.policy.target_prefix = "assets";
    input.assets.push_back(Asset(
        "logical-a",
        "physical-a",
        "album/photo.jpg",
        "assets/album/photo.jpg"));
    auto plan = photobridge::CanonicalMinimalPlan::Build(std::move(input));
    EXPECT_TRUE(plan.ok());
    return std::move(plan.value());
}

}  // namespace

TEST(PlanArtifactTest, WritesFixedJsonLinesSections)
{
    const auto plan = MakePlan();
    const auto result = photobridge::WriteFrozenPlan(plan, "plan-a");

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().plan_id, "plan-a");
    EXPECT_EQ(result.value().bytes.back(), '\n');
    EXPECT_NE(
        result.value().bytes.find("\"record\":\"header\""),
        std::string::npos);
    EXPECT_NE(
        result.value().bytes.find("canonical-plan-semantic-v1"),
        std::string::npos);
    EXPECT_NE(
        result.value().bytes.find("\"record\":\"asset\""),
        std::string::npos);
    EXPECT_NE(
        result.value().bytes.find("\"section\":\"outputs\""),
        std::string::npos);
    EXPECT_NE(
        result.value().bytes.find("\"section\":\"losses\""),
        std::string::npos);
    EXPECT_NE(
        result.value().bytes.find("\"record\":\"end\""),
        std::string::npos);
}

TEST(PlanArtifactTest, RoundTripsTypedPlanAndRawPathBytes)
{
    photobridge::MinimalPlanInput input;
    input.source_manifest_id = "manifest-1";
    input.target_root = std::string("/target/") + std::string(1, '\xFF');
    input.capabilities = photobridge::LocalDirectoryCapabilitiesV1();
    const std::string raw_path =
        std::string("album/") + std::string(1, '\xFF') + ".jpg";
    input.assets.push_back(Asset(
        "logical-a",
        "physical-a",
        raw_path,
        raw_path));
    auto plan = photobridge::CanonicalMinimalPlan::Build(std::move(input));
    ASSERT_TRUE(plan.ok());

    const auto written = photobridge::WriteFrozenPlan(
        plan.value(),
        "plan-a");
    ASSERT_TRUE(written.ok());
    const auto read = photobridge::ReadFrozenPlan(written.value().bytes);

    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value().plan_id, written.value().plan_id);
    EXPECT_EQ(read.value().bytes, written.value().bytes);
    EXPECT_EQ(
        read.value().artifact_digest,
        written.value().artifact_digest);
    EXPECT_EQ(
        read.value().semantic_digest,
        written.value().semantic_digest);
    ASSERT_EQ(read.value().plan.assets().size(), 1U);
    EXPECT_EQ(
        read.value().plan.assets()[0].source_path.bytes(),
        std::string("album/") + std::string(1, '\xFF') + ".jpg");
    EXPECT_EQ(
        read.value().plan.target_root(),
        std::string("/target/") + std::string(1, '\xFF'));
}

TEST(PlanArtifactTest, SeparatesArtifactAndSemanticDigests)
{
    const auto plan = MakePlan();
    const auto first = photobridge::WriteFrozenPlan(plan, "plan-a");
    const auto second = photobridge::WriteFrozenPlan(plan, "plan-b");

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_NE(first.value().bytes, second.value().bytes);
    EXPECT_NE(first.value().artifact_digest, second.value().artifact_digest);
    EXPECT_EQ(first.value().semantic_digest, second.value().semantic_digest);
    EXPECT_EQ(
        first.value().semantic_digest,
        photobridge::SemanticDigestFor(plan));
    EXPECT_EQ(
        first.value().artifact_digest,
        photobridge::ArtifactDigestFor(first.value().bytes));
}

TEST(PlanArtifactTest, SerializesAssetInputInCanonicalOrder)
{
    auto make = [](bool reverse) {
        photobridge::MinimalPlanInput input;
        input.source_manifest_id = "manifest-1";
        input.target_root = "/target/root";
        input.capabilities = photobridge::LocalDirectoryCapabilitiesV1();
        const auto first = Asset(
            "logical-a", "physical-a", "a/photo.jpg", "a/photo.jpg");
        const auto second = Asset(
            "logical-b", "physical-b", "b/photo.jpg", "b/photo.jpg");
        if (reverse) {
            input.assets = {second, first};
        } else {
            input.assets = {first, second};
        }
        return photobridge::CanonicalMinimalPlan::Build(std::move(input));
    };
    const auto first = make(false);
    const auto second = make(true);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    const auto first_file = photobridge::WriteFrozenPlan(first.value(), "plan");
    const auto second_file = photobridge::WriteFrozenPlan(second.value(), "plan");
    ASSERT_TRUE(first_file.ok());
    ASSERT_TRUE(second_file.ok());
    EXPECT_EQ(first_file.value().bytes, second_file.value().bytes);
    EXPECT_EQ(first_file.value().semantic_digest, second_file.value().semantic_digest);
}

TEST(PlanArtifactTest, RejectsMalformedOrOutOfOrderRecords)
{
    const auto plan = MakePlan();
    const auto written = photobridge::WriteFrozenPlan(plan, "plan-a");
    ASSERT_TRUE(written.ok());

    std::string missing_lf = written.value().bytes;
    missing_lf.pop_back();
    std::string bad_header = written.value().bytes;
    bad_header[0] = '[';

    const auto missing_lf_result = photobridge::ReadFrozenPlan(missing_lf);
    const auto bad_header_result = photobridge::ReadFrozenPlan(bad_header);

    EXPECT_FALSE(missing_lf_result.ok());
    EXPECT_FALSE(bad_header_result.ok());
}

TEST(PlanArtifactTest, RejectsUnknownSemanticProjectionVersion)
{
    const auto written = photobridge::WriteFrozenPlan(MakePlan(), "plan-a");
    ASSERT_TRUE(written.ok());
    std::string changed = written.value().bytes;
    const std::string current = "canonical-plan-semantic-v1";
    const auto position = changed.find(current);
    ASSERT_NE(position, std::string::npos);
    changed.replace(position, current.size(), "canonical-plan-semantic-v2");
    const auto result = photobridge::ReadFrozenPlan(changed);
    EXPECT_EQ(result.status().code(), photobridge::StatusCode::kInvalidArgument);
}
