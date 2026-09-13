#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/model/association_graph.h"

namespace {

photobridge::AssociationEdge Composition(
    std::string lhs,
    std::string rhs,
    photobridge::AssociationStatus status =
        photobridge::AssociationStatus::kConfirmed)
{
    return photobridge::AssociationEdge{
        photobridge::EdgeKind::kComposition,
        std::move(lhs),
        std::move(rhs),
        photobridge::RelationType::kJsonSidecar,
        "fixture.composition.v1",
        photobridge::Evidence{"fixture.composition.v1", "explicit fixture"},
        status,
    };
}

photobridge::AssociationEdge Attachment(
    std::string lhs,
    std::string rhs)
{
    return photobridge::AssociationEdge{
        photobridge::EdgeKind::kMetadataAttachment,
        std::move(lhs),
        std::move(rhs),
        photobridge::RelationType::kJsonSidecar,
        "fixture.attachment.v1",
        photobridge::Evidence{"fixture.attachment.v1", "sidecar"},
        photobridge::AssociationStatus::kConfirmed,
    };
}

std::vector<std::vector<std::string>> Members(
    const std::vector<photobridge::LogicalAsset>& assets)
{
    std::vector<std::vector<std::string>> result;
    for (const auto& asset : assets) result.push_back(asset.members);
    std::sort(result.begin(), result.end());
    return result;
}

}  // namespace

TEST(AssociationGraphTest, OnlyConfirmedCompositionEdgesUnion)
{
    photobridge::AssociationGraph graph;
    ASSERT_TRUE(graph.AddVertex("a").ok());
    ASSERT_TRUE(graph.AddVertex("b").ok());
    ASSERT_TRUE(graph.AddVertex("c").ok());
    ASSERT_TRUE(graph.AddEdge(Attachment("b", "c")).ok());
    ASSERT_TRUE(graph.AddEdge(Composition("a", "b")).ok());
    ASSERT_TRUE(graph.AddEdge(Composition(
        "a", "c", photobridge::AssociationStatus::kAmbiguous)).ok());
    EXPECT_EQ(graph.edge_count(), 3U);

    const auto result = graph.BuildLogicalAssets();
    ASSERT_TRUE(result.ok()) << result.status().message();
    ASSERT_EQ(result.value().size(), 2U);
    EXPECT_EQ(Members(result.value()),
        (std::vector<std::vector<std::string>>{{"a", "b"}, {"c"}}));
}

TEST(AssociationGraphTest, BuildIsIndependentOfInsertionOrder)
{
    photobridge::AssociationGraph first;
    photobridge::AssociationGraph second;
    for (const auto& id : {"a", "b", "c", "d"}) {
        ASSERT_TRUE(first.AddVertex(id).ok());
    }
    for (const auto& id : {"d", "c", "b", "a"}) {
        ASSERT_TRUE(second.AddVertex(id).ok());
    }
    ASSERT_TRUE(first.AddEdge(Composition("a", "b")).ok());
    ASSERT_TRUE(first.AddEdge(Composition("c", "d")).ok());
    ASSERT_TRUE(second.AddEdge(Composition("d", "c")).ok());
    ASSERT_TRUE(second.AddEdge(Composition("b", "a")).ok());

    const auto first_assets = first.BuildLogicalAssets();
    const auto second_assets = second.BuildLogicalAssets();
    ASSERT_TRUE(first_assets.ok());
    ASSERT_TRUE(second_assets.ok());
    EXPECT_EQ(Members(first_assets.value()), Members(second_assets.value()));
    EXPECT_EQ(first_assets.value()[0].id, second_assets.value()[0].id);
}

TEST(AssociationGraphTest, AggregatesCompositionChainsCanonically)
{
    photobridge::AssociationGraph graph;
    for (const auto& id : {"c", "a", "b"}) {
        ASSERT_TRUE(graph.AddVertex(id).ok());
    }
    ASSERT_TRUE(graph.AddEdge(Composition("b", "c")).ok());
    ASSERT_TRUE(graph.AddEdge(Composition("a", "b")).ok());

    const auto assets = graph.BuildLogicalAssets();
    ASSERT_TRUE(assets.ok());
    ASSERT_EQ(assets.value().size(), 1U);
    EXPECT_EQ(
        assets.value()[0].members,
        (std::vector<std::string>{"a", "b", "c"}));
    const auto expected_id = photobridge::LogicalAssetIdForMembers(
        {"c", "a", "b"});
    ASSERT_TRUE(expected_id.ok());
    EXPECT_EQ(assets.value()[0].id, expected_id.value());
}

TEST(AssociationGraphTest, LogicalAssetIdIgnoresEdgeEvidence)
{
    photobridge::AssociationGraph first;
    photobridge::AssociationGraph second;
    ASSERT_TRUE(first.AddVertex("a").ok());
    ASSERT_TRUE(first.AddVertex("b").ok());
    ASSERT_TRUE(second.AddVertex("a").ok());
    ASSERT_TRUE(second.AddVertex("b").ok());
    auto first_edge = Composition("a", "b");
    auto second_edge = Composition("a", "b");
    second_edge.rule = "different.rule.v2";
    second_edge.evidence = photobridge::Evidence{
        "different.rule.v2", "different evidence"};
    ASSERT_TRUE(first.AddEdge(std::move(first_edge)).ok());
    ASSERT_TRUE(second.AddEdge(std::move(second_edge)).ok());

    const auto first_assets = first.BuildLogicalAssets();
    const auto second_assets = second.BuildLogicalAssets();
    ASSERT_TRUE(first_assets.ok());
    ASSERT_TRUE(second_assets.ok());
    ASSERT_EQ(first_assets.value().size(), 1U);
    ASSERT_EQ(second_assets.value().size(), 1U);
    EXPECT_EQ(first_assets.value()[0].id, second_assets.value()[0].id);
}

TEST(AssociationGraphTest, RejectsUnknownAndDuplicateVerticesOrEdges)
{
    photobridge::AssociationGraph graph;
    EXPECT_EQ(
        graph.AddVertex("").code(),
        photobridge::StatusCode::kInvalidArgument);
    ASSERT_TRUE(graph.AddVertex("a").ok());
    EXPECT_EQ(
        graph.AddVertex("a").code(),
        photobridge::StatusCode::kAlreadyExists);
    EXPECT_EQ(
        graph.AddEdge(Composition("a", "b")).code(),
        photobridge::StatusCode::kNotFound);
}
