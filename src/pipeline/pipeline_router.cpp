#include "photobridge/pipeline/pipeline_support.h"

namespace photobridge {

namespace {

bool RequiresInput(PipelineStage)
{
    return true;
}

}  // namespace

Status RunPipelineStage(
    PipelineStage stage,
    std::string workspace_path,
    std::string input_path,
    std::string target_path,
    std::size_t workers,
    std::size_t db_batch_size,
    CommandContext& context)
{
    auto layout = WorkspaceLayout::FromRoot(
        std::filesystem::path(workspace_path));
    if (!layout.ok()) {
        return layout.status();
    }

    if (RequiresInput(stage) && input_path.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "pipeline stage requires an input path");
    }

    switch (stage) {
    case PipelineStage::kScan:
        return pipeline::RunScanService(layout, input_path, context);
    case PipelineStage::kPlan:
        return pipeline::RunPlanService(
            layout, input_path, target_path, context);
    case PipelineStage::kMigrate:
        return pipeline::RunMigrationService(
            layout, input_path, workers, db_batch_size, context);
    case PipelineStage::kResume:
        return pipeline::RunRecoveryService(layout, input_path, context);
    case PipelineStage::kVerify:
        return pipeline::RunVerifyService(layout, input_path, context);
    case PipelineStage::kStatus:
        return pipeline::RunStatusService(layout, input_path, context);
    }
    return Status(StatusCode::kInvalidArgument, "unknown pipeline stage");
}

}  // namespace photobridge
