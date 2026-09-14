#include "photobridge/pipeline/pipeline_support.h"

namespace photobridge::pipeline {

class PlanService final {
public:
    static Status Execute(
        const StatusOr<WorkspaceLayout>& layout,
        const std::string& input_path,
        const std::string& target_path,
        CommandContext& context)
    {
        if (target_path.empty()) {
            return Status(
                StatusCode::kInvalidArgument,
                "plan stage requires a target root");
        }

        auto connection = SqliteConnection::Open(layout.value().database);
        if (!connection.ok()) {
            return connection.status();
        }
        const Status schema_status = EnsureSchema(connection.value());
        if (!schema_status.ok()) {
            return schema_status;
        }

        auto frozen_manifest = SqliteManifestBuilder::Reopen(
            connection.value(),
            input_path);
        if (!frozen_manifest.ok()) {
            return frozen_manifest.status();
        }
        auto manifest = frozen_manifest.value().frozen_manifest();
        if (!manifest.ok()) {
            return manifest.status();
        }

        auto physical_assets = ReadManifestAssets(
            connection.value(),
            manifest.value().manifest_id);
        if (!physical_assets.ok()) {
            return physical_assets.status();
        }

        std::vector<LogicalAsset> logical_assets;
        logical_assets.reserve(physical_assets.value().size());
        for (const PhysicalAsset& asset : physical_assets.value()) {
            auto logical = MapPhysicalAssetToLogicalAsset(asset);
            if (!logical.ok()) {
                return logical.status();
            }
            logical_assets.push_back(std::move(logical.value()));
        }

        const TargetCapabilities capabilities =
            LocalDirectoryCapabilitiesV1();
        auto loss_analysis = AnalyzeCapabilityLoss(capabilities);
        if (!loss_analysis.ok()) return loss_analysis.status();
        const MigrationPolicy policy{};
        TargetPathMapper mapper;
        auto mappings = mapper.MapAll(
            logical_assets,
            capabilities,
            policy);
        if (!mappings.ok()) {
            return mappings.status();
        }

        std::vector<MinimalPlanAsset> plan_assets;
        plan_assets.reserve(mappings.value().size());
        for (const PathMapping& mapping : mappings.value()) {
            const auto logical = std::find_if(
                logical_assets.begin(),
                logical_assets.end(),
                [&mapping](const LogicalAsset& candidate) {
                    return candidate.id == mapping.logical_asset_id;
                });
            if (logical == logical_assets.end()
                || !logical->source_path.has_value()
                || logical->members.size() != 1) {
                return Status(
                    StatusCode::kInternal,
                    "logical asset mapping cannot be bound to one source");
            }

            const auto physical = std::find_if(
                physical_assets.value().begin(),
                physical_assets.value().end(),
                [&logical](const PhysicalAsset& candidate) {
                    return PhysicalAssetIdFor(candidate)
                        == logical->members.front();
                });
            if (physical == physical_assets.value().end()) {
                return Status(
                    StatusCode::kInternal,
                    "logical asset member is missing from manifest");
            }

            plan_assets.push_back(MinimalPlanAsset{
                logical->id,
                logical->members.front(),
                logical->source_path.value(),
                mapping.target_path,
                physical->identity,
            });
        }

        auto plan = CanonicalMinimalPlan::Build(PlannerInput{
            manifest.value().manifest_id,
            manifest.value().manifest_digest,
            target_path,
            capabilities,
            policy,
            std::move(plan_assets),
        });
        if (!plan.ok()) {
            return plan.status();
        }

        const std::string plan_id = "plan-"
            + SemanticDigestFor(plan.value()).ToHex();
        auto artifact = WriteFrozenPlan(plan.value(), plan_id);
        if (!artifact.ok()) {
            return artifact.status();
        }

        const std::filesystem::path artifact_path = layout.value().plans
            / (plan_id + ".plan.jsonl");
        const Status write_status = WritePlanFile(
            artifact_path,
            artifact.value().bytes);
        if (!write_status.ok()) {
            return write_status;
        }

        context.out << "plan completed: " << artifact_path.string()
                    << " assets=" << artifact.value().plan.assets().size()
                    << " artifact_digest="
                    << artifact.value().artifact_digest.ToHex()
                    << " semantic_digest="
                    << artifact.value().semantic_digest.ToHex()
                    << " capability_losses="
                    << loss_analysis.value().losses.size()
                    << "\n";
        return Status::Ok();
    
    }
};


Status RunPlanService(
    const StatusOr<WorkspaceLayout>& layout,
    const std::string& input_path,
    const std::string& target_path,
    CommandContext& context)
{
    return PlanService::Execute(layout, input_path, target_path, context);
}

}  // namespace photobridge::pipeline
