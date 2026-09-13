#pragma once

#include <cstdint>
#include <string>

#include "photobridge/cli/command.h"

namespace photobridge {

class ServeCommand final : public Command {
public:
    ServeCommand(
        std::string workspace_path,
        std::string bind_address,
        std::uint16_t port,
        bool bootstrap_only);

    Status Execute(CommandContext& context) override;

private:
    std::string workspace_path_;
    std::string bind_address_;
    std::uint16_t port_;
    bool bootstrap_only_;
};

}  // namespace photobridge
