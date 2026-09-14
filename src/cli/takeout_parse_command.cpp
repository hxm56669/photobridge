#include "photobridge/cli/takeout_parse_command.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <algorithm>

#include <nlohmann/json.hpp>

#include "photobridge/model/association_edge.h"
#include "photobridge/model/association_graph.h"
#include "photobridge/model/metadata_resolver.h"
#include "photobridge/model/photo_ir.h"
#include "photobridge/model/provenance.h"
#include "photobridge/source/takeout_parser.h"

namespace photobridge {
namespace {

struct MetadataSummary {
    std::size_t resolutions = 0;
    std::size_t provenance_records = 0;
    std::size_t conflicts = 0;
};

StatusOr<MetadataSummary> ResolveParsedMetadata(
    const std::vector<LogicalAsset>& logical_assets,
    const std::vector<MetadataCandidate>& candidates)
{
    constexpr std::array<MetadataField, 4> kFields{
        MetadataField::kTitle,
        MetadataField::kDescription,
        MetadataField::kFavorite,
        MetadataField::kTakenTime,
    };
    const auto& ruleset = GoogleTakeoutMetadataRuleset();
    MetadataResolver resolver;
    MetadataSummary summary;
    for (const LogicalAsset& logical_asset : logical_assets) {
        for (const MetadataField field : kFields) {
            std::vector<MetadataCandidate> group;
            for (const MetadataCandidate& candidate : candidates) {
                if (candidate.field != field
                    || std::find(
                           logical_asset.members.begin(),
                           logical_asset.members.end(),
                           candidate.asset_id)
                        == logical_asset.members.end()) {
                    continue;
                }
                group.push_back(candidate);
            }
            if (group.empty()) continue;

            auto resolution = resolver.Resolve(field, group, ruleset);
            if (!resolution.ok()) return resolution.status();
            auto provenance = BuildProvenance(resolution.value());
            if (!provenance.ok()) return provenance.status();
            ++summary.resolutions;
            summary.provenance_records += provenance.value().size();
            summary.conflicts += resolution.value().conflict ? 1U : 0U;
        }
    }
    return summary;
}

}  // namespace

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

    auto metadata = ResolveParsedMetadata(
        logical_assets.value(),
        parsed.value().candidates);
    if (!metadata.ok()) return metadata.status();

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
                << " photo_ir=" << photos.size()
                << " metadata_resolutions=" << metadata.value().resolutions
                << " metadata_provenance="
                << metadata.value().provenance_records
                << " metadata_conflicts=" << metadata.value().conflicts
                << "\n";
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
