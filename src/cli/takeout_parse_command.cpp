#include "photobridge/cli/takeout_parse_command.h"

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "photobridge/model/association_edge.h"
#include "photobridge/model/association_graph.h"
#include "photobridge/model/photo_ir.h"
#include "photobridge/source/takeout_parser.h"

namespace photobridge {

TakeoutParseCommand::TakeoutParseCommand(
    std::string root_path,
    std::string error_report_path)
    : root_path_(std::move(root_path)),
      error_report_path_(std::move(error_report_path))
{
}

Status WriteErrorReport(
    const std::filesystem::path& path,
    const std::vector<TakeoutParserError>& errors)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Status(
            StatusCode::kIoError,
            "create parser error report: " + path.string());
    }
    for (const TakeoutParserError& error : errors) {
        nlohmann::json record{
            {"code", error.code},
            {"message", error.message},
            {"path", error.relative_path.DisplayString()},
        };
        output << record.dump() << '\n';
        if (!output) {
            return Status(
                StatusCode::kIoError,
                "write parser error report: " + path.string());
        }
    }
    return Status::Ok();
}

Status TakeoutParseCommand::Execute(CommandContext& context)
{
    if (root_path_.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "parse requires a Takeout root path");
    }

    TakeoutParser parser;
    auto parsed = parser.Parse(std::filesystem::path(root_path_));
    if (!parsed.ok()) {
        return parsed.status();
    }

    if (!error_report_path_.empty()) {
        const Status report_status = WriteErrorReport(
            std::filesystem::path(error_report_path_),
            parsed.value().errors);
        if (!report_status.ok()) {
            return report_status;
        }
    }

    AssociationGraph graph;
    for (const PhysicalAsset& asset : parsed.value().assets) {
        const Status status = graph.AddVertex(PhysicalAssetIdFor(asset));
        if (!status.ok()) return status;
    }
    std::vector<AssociationEdge> edges;
    edges.reserve(parsed.value().relations.size());
    for (const SourceRelationCandidate& relation : parsed.value().relations) {
        edges.push_back(AssociationEdgeFor(relation));
        const Status status = graph.AddEdge(edges.back());
        if (!status.ok()) return status;
    }
    auto logical_assets = graph.BuildLogicalAssets();
    if (!logical_assets.ok()) return logical_assets.status();

    std::vector<CanonicalPhoto> photos;
    photos.reserve(logical_assets.value().size());
    for (const LogicalAsset& logical_asset : logical_assets.value()) {
        auto photo = BuildCanonicalPhoto(
            logical_asset,
            parsed.value().assets,
            edges);
        if (!photo.ok()) return photo.status();
        photos.push_back(std::move(photo.value()));
    }

    context.out << "parse completed: root=" << root_path_
                << " assets=" << parsed.value().assets.size()
                << " candidates=" << parsed.value().candidates.size()
                << " relations=" << parsed.value().relations.size()
                << " edges=" << parsed.value().relations.size()
                << " logical_assets=" << logical_assets.value().size()
                << " errors=" << parsed.value().errors.size()
                << " photo_ir=" << photos.size() << "\n";
    if (!error_report_path_.empty()) {
        context.out << "parser error report: " << error_report_path_ << "\n";
    }
    for (const TakeoutParserError& error : parsed.value().errors) {
        context.out << "parser error: path="
                    << error.relative_path.bytes()
                    << " code=" << error.code
                    << " message=" << error.message << "\n";
    }

    if (!parsed.value().errors.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "Takeout parse produced parser errors");
    }
    return Status::Ok();
}

}  // namespace photobridge
