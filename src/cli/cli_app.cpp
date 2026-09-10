#include "photobridge/cli/cli_app.h"

#include <memory>
#include <string>

#include <CLI/CLI.hpp>

#include "photobridge/app/workspace_service.h"
#include "photobridge/cli/dispatcher.h"
#include "photobridge/cli/init_command.h"
#include "photobridge/cli/scan_command.h"

namespace photobridge {

int ExitCodeForStatus(const Status& status) noexcept
{
    switch (status.code()) {
    case StatusCode::kOk:
        return 0;
    case StatusCode::kInvalidArgument:
        return 2;
    case StatusCode::kNotFound:
        return 3;
    case StatusCode::kAlreadyExists:
        return 4;
    case StatusCode::kPermissionDenied:
        return 5;
    case StatusCode::kIoError:
        return 6;
    case StatusCode::kInternal:
        return 7;
    }

    return 7;
}

int RunCli(
    int argc,
    char* argv[],
    std::ostream& out,
    std::ostream& err)
{
    CLI::App app{"PhotoBridge migration tool"};
    auto scan_command = app.add_subcommand(
        "scan",
        "scan source files");
    auto init_command = app.add_subcommand(
        "init",
        "initialize a PhotoBridge workspace");
    std::string workspace_path;
    init_command->add_option(
        "--workspace",
        workspace_path,
        "workspace root path")
        ->required();

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e, err);
    }

    WorkspaceService workspace_service;
    CommandDispatcher dispatcher;
    std::string selected_command;

    if (init_command->parsed()) {
        selected_command = "init";
        dispatcher.Register(
            selected_command,
            std::make_unique<InitCommand>(workspace_path));
    } else if (scan_command->parsed()) {
        selected_command = "scan";
        dispatcher.Register(
            selected_command,
            std::make_unique<ScanCommand>());
    }

    if (!selected_command.empty()) {
        CommandContext context{out, err, workspace_service};
        const Status status = dispatcher.Dispatch(
            selected_command,
            context);
        return ExitCodeForStatus(status);
    }

    return 0;
}

}  // namespace photobridge
