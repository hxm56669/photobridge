#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/model/relation_verifier.h"

namespace {

photobridge::PhotoRelationship Relation(const char* rhs)
{
    return {
        "lhs",
        rhs,
        photobridge::RelationType::kRawJpegPair,
        photobridge::AssociationStatus::kConfirmed,
        "rule-v1",
        {"rule-v1", "paired"},
    };
}

}  // namespace

TEST(RelationVerifierTest, DistinguishesMatchMismatchAndUnavailable)
{
    const std::vector<photobridge::PhotoRelationship> expected{Relation("rhs")};
    const std::vector<photobridge::PhotoRelationship> same{Relation("rhs")};
    const std::vector<photobridge::PhotoRelationship> changed{Relation("other")};
    const auto identical = photobridge::VerifyRelations(
        std::span<const photobridge::PhotoRelationship>(expected),
        std::span<const photobridge::PhotoRelationship>(same));
    EXPECT_EQ(
        identical.status,
        photobridge::RelationVerification::kIdentical);

    const auto mismatch = photobridge::VerifyRelations(
        std::span<const photobridge::PhotoRelationship>(expected),
        std::span<const photobridge::PhotoRelationship>(changed));
    EXPECT_EQ(
        mismatch.status,
        photobridge::RelationVerification::kMismatch);

    const auto unavailable = photobridge::VerifyRelations(
        std::nullopt,
        std::span<const photobridge::PhotoRelationship>(same));
    EXPECT_EQ(
        unavailable.status,
        photobridge::RelationVerification::kUnverifiable);
}
