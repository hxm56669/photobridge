#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "photobridge/cli/cli_app.h"
#include "photobridge/model/association_edge.h"
#include "photobridge/source/takeout_parser.h"

namespace {

void WriteFile(
    const std::filesystem::path& path,
    std::string_view bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    ASSERT_TRUE(file);
    file << bytes;
    ASSERT_TRUE(file);
}

std::vector<std::string> AssetSummary(
    const photobridge::TakeoutParseResult& result)
{
    std::vector<std::string> summary;
    for (const auto& asset : result.assets) {
        summary.push_back(
            std::string(asset.relative_path.bytes())
            + ":" + std::to_string(static_cast<int>(asset.kind)));
    }
    return summary;
}

std::vector<std::string> ErrorSummary(
    const photobridge::TakeoutParseResult& result)
{
    std::vector<std::string> summary;
    for (const auto& error : result.errors) {
        summary.push_back(
            std::string(error.relative_path.bytes())
            + ":" + error.code);
    }
    return summary;
}

}  // namespace

TEST(TakeoutParserTest, ClassifiesFixtureAndReportsParserErrors)
{
    const auto root = std::filesystem::temp_directory_path()
        / "photobridge_takeout_parser_fixture";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    WriteFile(root / "Trips" / "Photo.JPG", "image");
    WriteFile(
        root / "Trips" / "Photo.json",
        R"({"title":"trip","description":"walk","favorited":true,"photoTakenTime":{"timestamp":"1700000000"}})");
    WriteFile(root / "Metadata.json", R"({"name":"album"})");
    WriteFile(root / "broken.json", "{broken");
    WriteFile(root / "notes.txt", "not media");

    photobridge::TakeoutParser parser;
    const auto result = parser.Parse(root);

    ASSERT_TRUE(result.ok()) << result.status().message();
    ASSERT_EQ(result.value().assets.size(), 5U);
    EXPECT_EQ(result.value().assets[0].relative_path.bytes(), "Metadata.json");
    EXPECT_EQ(result.value().assets[0].kind, photobridge::AssetKind::kAlbumMetadata);
    EXPECT_EQ(result.value().assets[1].relative_path.bytes(), "Trips/Photo.JPG");
    EXPECT_EQ(result.value().assets[1].kind, photobridge::AssetKind::kMedia);
    EXPECT_EQ(result.value().assets[2].relative_path.bytes(), "Trips/Photo.json");
    EXPECT_EQ(result.value().assets[2].kind, photobridge::AssetKind::kSidecarJson);
    EXPECT_EQ(result.value().assets[3].relative_path.bytes(), "broken.json");
    EXPECT_EQ(result.value().assets[3].kind, photobridge::AssetKind::kSidecarJson);
    EXPECT_EQ(result.value().assets[4].relative_path.bytes(), "notes.txt");
    EXPECT_EQ(result.value().assets[4].kind, photobridge::AssetKind::kUnknown);
    ASSERT_EQ(result.value().errors.size(), 3U);
    EXPECT_EQ(result.value().errors[0].relative_path.bytes(), "broken.json");
    EXPECT_EQ(result.value().errors[0].code, "invalid_json");
    EXPECT_EQ(result.value().errors[1].relative_path.bytes(), "broken.json");
    EXPECT_EQ(result.value().errors[1].code, "orphan_sidecar");
    EXPECT_EQ(result.value().errors[2].relative_path.bytes(), "notes.txt");
    EXPECT_EQ(result.value().errors[2].code, "unknown_file_kind");
    ASSERT_EQ(result.value().candidates.size(), 4U);
    EXPECT_EQ(
        result.value().candidates[0].asset_id,
        photobridge::PhysicalAssetIdFor(result.value().assets[2]));
    EXPECT_EQ(
        result.value().candidates[0].field,
        photobridge::MetadataField::kTitle);
    ASSERT_TRUE(std::holds_alternative<std::string>(
        result.value().candidates[0].value));
    EXPECT_EQ(
        std::get<std::string>(result.value().candidates[0].value),
        "trip");
    EXPECT_EQ(
        result.value().candidates[0].extraction_rule,
        "takeout.json.title.v1");
    EXPECT_EQ(
        result.value().candidates[0].evidence,
        "Trips/Photo.json#/title");
    EXPECT_EQ(
        result.value().candidates[3].field,
        photobridge::MetadataField::kTakenTime);
    ASSERT_TRUE(std::holds_alternative<photobridge::TimeCandidate>(
        result.value().candidates[3].value));
    const auto& time = std::get<photobridge::TimeCandidate>(
        result.value().candidates[3].value);
    ASSERT_TRUE(std::holds_alternative<photobridge::AbsoluteTime>(time.value));
    EXPECT_EQ(
        std::get<photobridge::AbsoluteTime>(time.value).unix_ns,
        1'700'000'000'000'000'000LL);

    std::filesystem::remove_all(root, cleanup_error);
}

TEST(TakeoutParserTest, OutputIsStableAcrossCreationOrder)
{
    const auto first = std::filesystem::temp_directory_path()
        / "photobridge_takeout_parser_first";
    const auto second = std::filesystem::temp_directory_path()
        / "photobridge_takeout_parser_second";
    std::error_code cleanup_error;
    std::filesystem::remove_all(first, cleanup_error);
    std::filesystem::remove_all(second, cleanup_error);
    ASSERT_TRUE(std::filesystem::create_directories(first));
    ASSERT_TRUE(std::filesystem::create_directories(second));

    WriteFile(first / "b.jpg", "b");
    WriteFile(first / "a.json", "{}");
    WriteFile(first / "c.txt", "c");
    WriteFile(second / "c.txt", "c");
    WriteFile(second / "a.json", "{}");
    WriteFile(second / "b.jpg", "b");

    photobridge::TakeoutParser parser;
    const auto first_result = parser.Parse(first);
    const auto second_result = parser.Parse(second);
    ASSERT_TRUE(first_result.ok());
    ASSERT_TRUE(second_result.ok());
    EXPECT_EQ(AssetSummary(first_result.value()), AssetSummary(second_result.value()));
    EXPECT_EQ(ErrorSummary(first_result.value()), ErrorSummary(second_result.value()));

    std::filesystem::remove_all(first, cleanup_error);
    std::filesystem::remove_all(second, cleanup_error);
}

TEST(TakeoutParserTest, MissingRootIsAnError)
{
    photobridge::TakeoutParser parser;
    const auto result = parser.Parse(
        std::filesystem::temp_directory_path()
        / "photobridge_takeout_parser_missing");

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), photobridge::StatusCode::kNotFound);
}

TEST(TakeoutParserTest, InvalidCandidateFieldsBecomeParserErrors)
{
    const auto root = std::filesystem::temp_directory_path()
        / "photobridge_takeout_parser_field_errors";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    WriteFile(root / "photo.jpg", "image");
    WriteFile(
        root / "photo.json",
        R"({"title":7,"favorited":"yes","photoTakenTime":{"timestamp":"not-a-time"}})");

    photobridge::TakeoutParser parser;
    const auto result = parser.Parse(root);

    ASSERT_TRUE(result.ok()) << result.status().message();
    ASSERT_EQ(result.value().candidates.size(), 0U);
    ASSERT_EQ(result.value().errors.size(), 3U);
    EXPECT_EQ(result.value().errors[0].code, "metadata_field_type");
    EXPECT_EQ(result.value().errors[1].code, "metadata_field_type");
    EXPECT_EQ(result.value().errors[2].code, "metadata_field_value");

    std::filesystem::remove_all(root, cleanup_error);
}

TEST(TakeoutParserTest, UsesStableMediaAndSidecarClassificationContract)
{
    const auto root = std::filesystem::temp_directory_path()
        / "photobridge_takeout_parser_classification";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    WriteFile(root / "IMG_0001.ARW", "raw");
    WriteFile(root / "IMG_0001.JPEG", "jpeg");
    WriteFile(root / "IMG_0001.MOV", "video");
    WriteFile(root / "IMG_0001.XMP", "<x:xmpmeta/>");
    WriteFile(root / "Metadata.JSON", "{}");
    ASSERT_TRUE(std::filesystem::create_directories(root / "empty"));

    photobridge::TakeoutParser parser;
    const auto result = parser.Parse(root);

    ASSERT_TRUE(result.ok()) << result.status().message();
    ASSERT_EQ(result.value().assets.size(), 5U);
    EXPECT_EQ(result.value().assets[0].relative_path.bytes(), "IMG_0001.ARW");
    EXPECT_EQ(result.value().assets[0].kind, photobridge::AssetKind::kMedia);
    EXPECT_EQ(result.value().assets[1].relative_path.bytes(), "IMG_0001.JPEG");
    EXPECT_EQ(result.value().assets[1].kind, photobridge::AssetKind::kMedia);
    EXPECT_EQ(result.value().assets[2].relative_path.bytes(), "IMG_0001.MOV");
    EXPECT_EQ(result.value().assets[2].kind, photobridge::AssetKind::kMedia);
    EXPECT_EQ(result.value().assets[3].relative_path.bytes(), "IMG_0001.XMP");
    EXPECT_EQ(result.value().assets[3].kind, photobridge::AssetKind::kSidecarXmp);
    EXPECT_EQ(result.value().assets[4].relative_path.bytes(), "Metadata.JSON");
    EXPECT_EQ(result.value().assets[4].kind, photobridge::AssetKind::kAlbumMetadata);
    EXPECT_TRUE(result.value().candidates.empty());
    ASSERT_EQ(result.value().relations.size(), 2U);
    EXPECT_EQ(
        result.value().relations[0].relation,
        photobridge::SourceRelationKind::kRawJpegPair);
    EXPECT_EQ(
        result.value().relations[1].relation,
        photobridge::SourceRelationKind::kLivePhotoMotionForMedia);
    ASSERT_EQ(result.value().errors.size(), 1U);
    EXPECT_EQ(result.value().errors[0].relative_path.bytes(), "IMG_0001.XMP");
    EXPECT_EQ(result.value().errors[0].code, "ambiguous_sidecar_relation");

    std::filesystem::remove_all(root, cleanup_error);
}

TEST(TakeoutParserTest, GeneratesOnlyUnambiguousBasenameRelations)
{
    const auto root = std::filesystem::temp_directory_path()
        / "photobridge_takeout_parser_relations";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    ASSERT_TRUE(std::filesystem::create_directories(root / "album"));
    WriteFile(root / "album" / "photo.jpg", "photo");
    WriteFile(root / "album" / "photo.json", "{}");
    WriteFile(root / "album" / "orphan.json", "{}");
    WriteFile(root / "album" / "ambiguous.jpg", "jpg");
    WriteFile(root / "album" / "ambiguous.heic", "heic");
    WriteFile(root / "album" / "ambiguous.json", "{}");
    WriteFile(root / "album" / "without-sidecar.jpg", "photo");

    photobridge::TakeoutParser parser;
    const auto result = parser.Parse(root);

    ASSERT_TRUE(result.ok()) << result.status().message();
    ASSERT_EQ(result.value().relations.size(), 1U);
    EXPECT_EQ(result.value().relations[0].rule, "takeout.basename.v1");
    EXPECT_EQ(
        result.value().relations[0].evidence,
        "album/photo.json matches album/photo.jpg");
    EXPECT_EQ(
        result.value().relations[0].relation,
        photobridge::SourceRelationKind::kJsonSidecarForMedia);
    const auto edge = photobridge::AssociationEdgeFor(
        result.value().relations[0]);
    EXPECT_EQ(edge.edge_kind, photobridge::EdgeKind::kMetadataAttachment);
    EXPECT_EQ(edge.status, photobridge::AssociationStatus::kConfirmed);
    EXPECT_EQ(edge.relation, photobridge::RelationType::kJsonSidecar);
    EXPECT_EQ(edge.rule, "takeout.basename.v1");
    EXPECT_EQ(edge.evidence.detail, "album/photo.json matches album/photo.jpg");
    ASSERT_EQ(result.value().errors.size(), 3U);
    EXPECT_EQ(result.value().errors[0].relative_path.bytes(), "album/ambiguous.json");
    EXPECT_EQ(result.value().errors[0].code, "ambiguous_sidecar_relation");
    EXPECT_EQ(result.value().errors[1].relative_path.bytes(), "album/orphan.json");
    EXPECT_EQ(result.value().errors[1].code, "orphan_sidecar");
    EXPECT_EQ(result.value().errors[2].relative_path.bytes(), "album/without-sidecar.jpg");
    EXPECT_EQ(result.value().errors[2].code, "missing_sidecar");

    std::filesystem::remove_all(root, cleanup_error);
}

TEST(TakeoutParserTest, SeparatesExtendedSidecarNamesFromTruncatedMatches)
{
    const auto root = std::filesystem::temp_directory_path()
        / "photobridge_takeout_parser_truncation";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    WriteFile(root / "photo.jpg", "photo");
    WriteFile(root / "photo.jpg.json", "{}");
    WriteFile(root / "very-long-photo-name.jpg", "photo");
    WriteFile(root / "very-long-photo.json", "{}");

    photobridge::TakeoutParser parser;
    const auto result = parser.Parse(root);

    ASSERT_TRUE(result.ok()) << result.status().message();
    ASSERT_EQ(result.value().relations.size(), 1U);
    EXPECT_EQ(
        result.value().relations[0].rule,
        "takeout.sidecar-media-extension.v1");
    EXPECT_EQ(
        result.value().relations[0].evidence,
        "photo.jpg.json matches photo.jpg");
    ASSERT_EQ(result.value().errors.size(), 1U);
    EXPECT_EQ(
        result.value().errors[0].relative_path.bytes(),
        "very-long-photo.json");
    EXPECT_EQ(result.value().errors[0].code, "truncated_basename_match");

    std::filesystem::remove_all(root, cleanup_error);
}

TEST(TakeoutParserCliTest, PrintsDiagnosticErrorsAndReturnsInvalidArgument)
{
    const auto root = std::filesystem::temp_directory_path()
        / "photobridge_takeout_parser_cli_fixture";
    const auto report = std::filesystem::temp_directory_path()
        / "photobridge_takeout_parser_cli_errors.jsonl";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::remove(report, cleanup_error);
    ASSERT_TRUE(std::filesystem::create_directories(root));
    WriteFile(root / "photo.jpg", "image");
    WriteFile(root / "photo.json", "not json");

    std::vector<std::string> args{
        "photobridge", "parse", "--takeout", root.string(),
        "--error-report", report.string()};
    std::vector<char*> argv;
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    std::ostringstream out;
    std::ostringstream err;
    const int exit_code = photobridge::RunCli(
        static_cast<int>(argv.size()),
        argv.data(),
        out,
        err);

    EXPECT_EQ(exit_code, 2);
    EXPECT_NE(out.str().find("assets=2 candidates=0 relations=1 edges=1 logical_assets=2 errors=1"), std::string::npos);
    EXPECT_NE(out.str().find("path=photo.json code=invalid_json"), std::string::npos);
    std::ifstream report_file(report, std::ios::binary);
    ASSERT_TRUE(report_file);
    const std::string report_bytes{
        std::istreambuf_iterator<char>(report_file),
        std::istreambuf_iterator<char>()};
    const auto report_record = nlohmann::json::parse(report_bytes);
    EXPECT_EQ(report_record.at("code"), "invalid_json");
    EXPECT_EQ(report_record.at("path"), "photo.json");
    EXPECT_NE(
        report_record.at("message").get<std::string>().find("parse error"),
        std::string::npos);
    EXPECT_TRUE(err.str().empty());

    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::remove(report, cleanup_error);
}
