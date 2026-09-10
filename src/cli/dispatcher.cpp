#include "photobridge/cli/dispatcher.h"

#include "photobridge/cli/command.h"

namespace photobridge {

void CommandDispatcher::Register(
    std::string name,
    std::unique_ptr<Command> command)
{
    commands_.emplace(
        std::move(name),
        std::move(command));
}


Status CommandDispatcher::Dispatch(
    const std::string& name,
    CommandContext& context)
{
    auto it = commands_.find(name);

    if(it == commands_.end())
    {
        return Status(
            StatusCode::kNotFound,
            "command not found: " + name);
    }

    return it->second->Execute(context);
}

} // namespace photobridge
