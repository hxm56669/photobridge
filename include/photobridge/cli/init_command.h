#pragma once

#include <string>

#include "photobridge/cli/command.h"

namespace photobridge {

class InitCommand : public Command {
public:
    explicit InitCommand(std::string workspace_path);

    Status Execute(CommandContext& context) override;

private:
    std::string workspace_path_;
};

}  // namespace photobridge
