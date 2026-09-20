#pragma once

#include <string>
#include <cstddef>

#include "photobridge/cli/pipeline_command.h"

namespace photobridge {

Status RunPipelineStage(
    PipelineStage stage,
    std::string workspace_path,
    std::string input_path,
    std::string target_path,
    std::size_t workers,
    CommandContext& context);

}  // namespace photobridge
