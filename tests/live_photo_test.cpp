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
    ASSERT_TRUE(file);
}

}  // namespace

TEST(LivePhotoTest, UniqueStillAndMotionBecomeOneComposition)
{
    const auto root = std::filesystem::temp_directory_path()
        / "photobridge_live_photo_fixture";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    WriteFile(root / "IMG_0001.JPG", "still");
    WriteFile(root / "IMG_0001.MOV", "motion");

    photobridge::TakeoutParser parser;
    const auto parsed = parser.Parse(root);
    ASSERT_TRUE(parsed.ok()) << parsed.status().message();
    ASSERT_EQ(parsed.value().relations.size(), 1U);
    EXPECT_EQ(
        parsed.value().relations[0].relation,
        photobridge::SourceRelationKind::kLivePhotoMotionForMedia);
    EXPECT_EQ(parsed.value().relations[0].rule, "takeout.live-photo.v1");
    EXPECT_TRUE(parsed.value().errors.empty());

    photobridge::AssociationGraph graph;
    for (const auto& asset : parsed.value().assets) {
        ASSERT_TRUE(graph.AddVertex(photobridge::PhysicalAssetIdFor(asset)).ok());
    }
    ASSERT_TRUE(graph.AddEdge(photobridge::AssociationEdgeFor(
        parsed.value().relations[0])).ok());
    const auto logical_assets = graph.BuildLogicalAssets();
    ASSERT_TRUE(logical_assets.ok());
    ASSERT_EQ(logical_assets.value().size(), 1U);
    EXPECT_EQ(logical_assets.value()[0].members.size(), 2U);

    std::filesystem::remove_all(root, cleanup_error);
}

TEST(LivePhotoTest, MultipleStillOrMotionFilesAreAmbiguous)
{
    const auto root = std::filesystem::temp_directory_path()
        / "photobridge_live_photo_ambiguous_fixture";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    WriteFile(root / "IMG_0001.JPG", "still");
    WriteFile(root / "IMG_0001.HEIC", "still");
    WriteFile(root / "IMG_0001.MOV", "motion");

    photobridge::TakeoutParser parser;
    const auto parsed = parser.Parse(root);
    ASSERT_TRUE(parsed.ok());
    EXPECT_TRUE(parsed.value().relations.empty());
    ASSERT_EQ(parsed.value().errors.size(), 3U);
    for (const auto& error : parsed.value().errors) {
        EXPECT_EQ(error.code, "ambiguous_live_photo_relation");
    }

    std::filesystem::remove_all(root, cleanup_error);
}
