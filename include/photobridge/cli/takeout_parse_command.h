#pragma once

#include <string>

#include "photobridge/cli/command.h"

namespace photobridge {

class TakeoutParseCommand final : public Command {
public:
    TakeoutParseCommand(
        std::string root_path,
        std::string error_report_path = {});

    Status Execute(CommandContext& context) override;

private:
    std::string root_path_;
    std::string error_report_path_;
};

}  // namespace photobridge
