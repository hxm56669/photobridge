#include "photobridge/cli/pipeline_command.h"

#include <utility>

#include "photobridge/cli/pipeline_services.h"

namespace photobridge {

PipelineCommand::PipelineCommand(
    PipelineStage stage,
    std::string workspace_path,
    std::string input_path,
    std::string target_path)
    : stage_(stage),
      workspace_path_(std::move(workspace_path)),
      input_path_(std::move(input_path)),
      target_path_(std::move(target_path))
{
}

Status PipelineCommand::Execute(CommandContext& context)
{
    return RunPipelineStage(
        stage_,
        workspace_path_,
        input_path_,
        target_path_,
        context);
}

}  // namespace photobridge
