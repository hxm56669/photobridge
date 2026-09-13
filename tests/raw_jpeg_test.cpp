#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include "photobridge/model/association_edge.h"
#include "photobridge/model/association_graph.h"
#include "photobridge/source/takeout_parser.h"

namespace {

void WriteFile(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream file(path, std::ios::binary);
    ASSERT_TRUE(file);
    file << bytes;
}

}  // namespace

TEST(RawJpegTest, UniquePairBecomesConfirmedComposition)
{
    const auto root = std::filesystem::temp_directory_path()
        / "photobridge_raw_jpeg_fixture";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    WriteFile(root / "capture.ARW", "raw");
    WriteFile(root / "capture.jpeg", "jpeg");

    photobridge::TakeoutParser parser;
    const auto parsed = parser.Parse(root);
    ASSERT_TRUE(parsed.ok());
    ASSERT_EQ(parsed.value().relations.size(), 1U);
    EXPECT_EQ(
        parsed.value().relations[0].relation,
        photobridge::SourceRelationKind::kRawJpegPair);
    EXPECT_EQ(parsed.value().relations[0].rule, "takeout.raw-jpeg.v1");
    EXPECT_TRUE(parsed.value().errors.empty());

    const auto edge = photobridge::AssociationEdgeFor(
        parsed.value().relations[0]);
    EXPECT_EQ(edge.edge_kind, photobridge::EdgeKind::kComposition);
    EXPECT_EQ(edge.relation, photobridge::RelationType::kRawJpegPair);
    EXPECT_EQ(edge.status, photobridge::AssociationStatus::kConfirmed);

    photobridge::AssociationGraph graph;
    for (const auto& asset : parsed.value().assets) {
        ASSERT_TRUE(graph.AddVertex(photobridge::PhysicalAssetIdFor(asset)).ok());
    }
    ASSERT_TRUE(graph.AddEdge(edge).ok());
    const auto logical_assets = graph.BuildLogicalAssets();
    ASSERT_TRUE(logical_assets.ok());
    ASSERT_EQ(logical_assets.value().size(), 1U);
    EXPECT_EQ(logical_assets.value()[0].members.size(), 2U);

    std::filesystem::remove_all(root, cleanup_error);
}
