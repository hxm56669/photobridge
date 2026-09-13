#include <gtest/gtest.h>

#include "photobridge/model/metadata_resolver.h"
#include "photobridge/model/provenance.h"

namespace {

photobridge::MetadataCandidate Candidate(
    std::string asset_id,
    std::string value)
{
    return photobridge::MetadataCandidate{
        std::move(asset_id),
        photobridge::MetadataField::kTitle,
        std::move(value),
        photobridge::MetadataSource::kGoogleTakeoutJson,
        "takeout.json.title.v1",
        "metadata.json#/title",
    };
}

}  // namespace

TEST(ProvenanceTest, ExplainsSelectedAndRejectedCandidates)
{
    const std::vector<photobridge::MetadataCandidate> candidates{
        Candidate("asset-b", "B"),
        Candidate("asset-a", "A"),
    };
    photobridge::MetadataResolver resolver;
    const auto resolution = resolver.Resolve(
        photobridge::MetadataField::kTitle,
        candidates,
        photobridge::GoogleTakeoutMetadataRuleset());
    ASSERT_TRUE(resolution.ok());
    const auto provenance = photobridge::BuildProvenance(resolution.value());
    ASSERT_TRUE(provenance.ok());
    ASSERT_EQ(provenance.value().size(), 2U);
    EXPECT_EQ(provenance.value()[0].asset_id, "asset-a");
    EXPECT_EQ(
        provenance.value()[0].outcome,
        photobridge::ProvenanceOutcome::kSelected);
    EXPECT_EQ(
        provenance.value()[1].outcome,
        photobridge::ProvenanceOutcome::kRejected);
    EXPECT_NE(provenance.value()[1].reason.find("tie-break"), std::string::npos);
}

TEST(ProvenanceTest, EmptyResolutionHasNoFabricatedProvenance)
{
    const auto resolution = photobridge::MetadataResolver().Resolve(
        photobridge::MetadataField::kTitle,
        std::vector<photobridge::MetadataCandidate>{},
        photobridge::GoogleTakeoutMetadataRuleset());
    ASSERT_TRUE(resolution.ok());
    const auto provenance = photobridge::BuildProvenance(resolution.value());
    ASSERT_TRUE(provenance.ok());
    EXPECT_TRUE(provenance.value().empty());
}
