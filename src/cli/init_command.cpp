#include "photobridge/cli/init_command.h"

#include <filesystem>
#include <utility>

#include "photobridge/app/workspace_layout.h"
#include "photobridge/app/workspace_service.h"

namespace photobridge {

InitCommand::InitCommand(std::string workspace_path)
    : workspace_path_(std::move(workspace_path))
{
}

Status InitCommand::Execute(CommandContext& context)
{
    auto layout = WorkspaceLayout::FromRoot(
        std::filesystem::path(workspace_path_));
    if (!layout.ok()) {
        return layout.status();
    }

    const Status status = context.workspace_service.Initialize(
        layout.value());
    if (!status.ok()) {
        return status;
    }

    context.out << "initialized workspace: "
                << layout.value().root.string() << "\n";
    return Status::Ok();
}

}  // namespace photobridge
