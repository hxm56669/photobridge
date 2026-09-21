#include "photobridge/cli/cli_app.h"

#include <memory>
#include <string>

#include <CLI/CLI.hpp>

#include "photobridge/app/workspace_service.h"
#include "photobridge/cli/dispatcher.h"
#include "photobridge/cli/init_command.h"
#include "photobridge/cli/pipeline_command.h"
#include "photobridge/cli/serve_command.h"
#include "photobridge/cli/takeout_parse_command.h"

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
    auto init_command = app.add_subcommand(
        "init",
        "initialize a PhotoBridge workspace");
    auto scan_command = app.add_subcommand(
        "scan",
        "scan source files");
    auto parse_command = app.add_subcommand(
        "parse",
        "parse a Google Takeout directory");
    auto plan_command = app.add_subcommand(
        "plan",
        "freeze a migration plan");
    auto migrate_command = app.add_subcommand(
        "migrate",
        "execute a frozen migration plan");
    auto resume_command = app.add_subcommand(
        "resume",
        "resume an unfinished migration plan");
    auto verify_command = app.add_subcommand(
        "verify",
        "verify migrated outputs");
    auto status_command = app.add_subcommand(
        "status",
        "show migration task status");
    auto serve_command = app.add_subcommand(
        "serve",
        "prepare a LAN upload receiver");
    std::string workspace_path;
    std::string input_path;
    std::string target_path;
    std::string takeout_path;
    std::string error_report_path;
    std::string bind_address;
    std::uint16_t port = 8787;
    std::size_t workers = 4;
    std::size_t db_batch_size = 8;
    bool bootstrap_only = false;
    init_command->add_option(
        "--workspace",
        workspace_path,
        "workspace root path")
        ->required();
    parse_command->add_option(
        "--takeout",
        takeout_path,
        "Google Takeout root path")
        ->required();
    parse_command->add_option(
        "--error-report",
        error_report_path,
        "deterministic JSONL parser error report path");
    auto add_pipeline_options = [&workspace_path, &input_path](
        CLI::App* command,
        const char* input_option,
        const char* input_description) {
        command->add_option(
            "--workspace",
            workspace_path,
            "workspace root path")
            ->required();
        command->add_option(
            input_option,
            input_path,
            input_description)
            ->required();
    };
    add_pipeline_options(
        scan_command,
        "--source",
        "source root path");
    add_pipeline_options(
    plan_command,
        "--manifest",
        "frozen source manifest id");
    plan_command->add_option(
        "--target",
        target_path,
        "target root path")
        ->required();
    add_pipeline_options(
        migrate_command,
        "--plan",
        "frozen plan path");
    migrate_command->add_option(
        "--workers", workers, "number of migration workers (1-8)")
        ->check(CLI::Range(1, 8))
        ->capture_default_str();
    migrate_command->add_option(
        "--db-batch-size", db_batch_size,
        "maximum opportunistic runtime DB batch size (1-16)")
        ->check(CLI::Range(1, 16))
        ->capture_default_str();
    add_pipeline_options(
        resume_command,
        "--plan",
        "frozen plan path");
    add_pipeline_options(
        verify_command,
        "--plan",
        "frozen plan path");
    add_pipeline_options(
        status_command,
        "--plan",
        "frozen plan path");
    serve_command->add_option(
        "--workspace",
        workspace_path,
        "workspace root path")
        ->required();
    serve_command->add_option(
        "--bind",
        bind_address,
        "LAN address to advertise and bind")
        ->capture_default_str();
    serve_command->add_option(
        "--port",
        port,
        "HTTP port")
        ->capture_default_str();
    serve_command->add_flag(
        "--bootstrap-only",
        bootstrap_only,
        "print startup information without listening");

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
    } else if (parse_command->parsed()) {
        selected_command = "parse";
        dispatcher.Register(
            selected_command,
            std::make_unique<TakeoutParseCommand>(
                takeout_path,
                error_report_path));
    } else if (scan_command->parsed()) {
        selected_command = "scan";
        dispatcher.Register(
            selected_command,
            std::make_unique<PipelineCommand>(
                PipelineStage::kScan,
                workspace_path,
                input_path));
    } else if (plan_command->parsed()) {
        selected_command = "plan";
        dispatcher.Register(
            selected_command,
            std::make_unique<PipelineCommand>(
                PipelineStage::kPlan,
                workspace_path,
                input_path,
                target_path));
    } else if (migrate_command->parsed()) {
        selected_command = "migrate";
        dispatcher.Register(
            selected_command,
            std::make_unique<PipelineCommand>(
                PipelineStage::kMigrate,
                workspace_path,
                input_path,
                std::string{},
                workers,
                db_batch_size));
    } else if (resume_command->parsed()) {
        selected_command = "resume";
        dispatcher.Register(
            selected_command,
            std::make_unique<PipelineCommand>(
                PipelineStage::kResume,
                workspace_path,
                input_path));
    } else if (verify_command->parsed()) {
        selected_command = "verify";
        dispatcher.Register(
            selected_command,
            std::make_unique<PipelineCommand>(
                PipelineStage::kVerify,
                workspace_path,
                input_path));
    } else if (status_command->parsed()) {
        selected_command = "status";
        dispatcher.Register(
            selected_command,
            std::make_unique<PipelineCommand>(
                PipelineStage::kStatus,
                workspace_path,
                input_path));
    } else if (serve_command->parsed()) {
        selected_command = "serve";
        dispatcher.Register(
            selected_command,
            std::make_unique<ServeCommand>(
                workspace_path,
                bind_address,
                port,
                bootstrap_only));
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
