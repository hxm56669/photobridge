#pragma once

#include <string>
#include <cstddef>

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
        std::string target_path = {},
        std::size_t workers = 4,
        std::size_t db_batch_size = 8);

    Status Execute(CommandContext& context) override;

private:
    PipelineStage stage_;
    std::string workspace_path_;
    std::string input_path_;
    std::string target_path_;
    std::size_t workers_;
    std::size_t db_batch_size_;
};

}  // namespace photobridge
