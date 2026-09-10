#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "photobridge/cli/command.h"

namespace photobridge {

class CommandDispatcher {
public:

    void Register(
        std::string name,
        std::unique_ptr<Command> command);

    Status Dispatch(
        const std::string& name,
        CommandContext& context);

private:

    std::unordered_map<
        std::string,
        std::unique_ptr<Command>>
        commands_;
};

} // namespace photobridge
