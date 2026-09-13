#pragma once

#include <string>

#include "photobridge/cli/command.h"

namespace photobridge {

enum class PipelineStage {
    kScan,
    kPlan,
    kMigrate,
    kResume,
    kVerify,
    kStatus,
};

class PipelineCommand final : public Command {
public:
    PipelineCommand(
        PipelineStage stage,
        std::string workspace_path,
        std::string input_path,
        std::string target_path = {});

    Status Execute(CommandContext& context) override;

private:
    PipelineStage stage_;
    std::string workspace_path_;
    std::string input_path_;
    std::string target_path_;
};

}  // namespace photobridge
