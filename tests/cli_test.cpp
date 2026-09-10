#include <memory>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/cli/cli_app.h"
#include "photobridge/cli/dispatcher.h"
#include "photobridge/cli/scan_command.h"
#include "photobridge/app/workspace_service.h"

namespace {

int RunCliWithArgs(
    std::vector<std::string> args,
    std::ostream& out,
    std::ostream& err)
{
    std::vector<char*> argv;
    argv.reserve(args.size());

    for (auto& arg : args) {
        argv.push_back(arg.data());
    }

    return photobridge::RunCli(
        static_cast<int>(argv.size()),
        argv.data(),
        out,
        err);
}

}  // namespace

TEST(ExitCodeForStatusTest, MapsStatusCodesToStableExitCodes)
{
    const std::vector<std::pair<photobridge::StatusCode, int>> cases{
        {photobridge::StatusCode::kOk, 0},
        {photobridge::StatusCode::kInvalidArgument, 2},
        {photobridge::StatusCode::kNotFound, 3},
        {photobridge::StatusCode::kAlreadyExists, 4},
        {photobridge::StatusCode::kPermissionDenied, 5},
        {photobridge::StatusCode::kIoError, 6},
        {photobridge::StatusCode::kInternal, 7},
    };

    for (const auto& [code, expected_exit_code] : cases) {
        EXPECT_EQ(
            photobridge::ExitCodeForStatus(
                photobridge::Status(code, "test")),
            expected_exit_code);
    }
}

TEST(CommandDispatcherTest, DispatchesCommandWithContext)
{
    photobridge::CommandDispatcher dispatcher;
    dispatcher.Register(
        "scan",
        std::make_unique<photobridge::ScanCommand>());

    std::ostringstream out;
    std::ostringstream err;
    photobridge::WorkspaceService workspace_service;
    photobridge::CommandContext context{out, err, workspace_service};

    const auto status = dispatcher.Dispatch("scan", context);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(out.str(), "scan command\n");
    EXPECT_TRUE(err.str().empty());
}

TEST(CommandDispatcherTest, MissingCommandReturnsNotFound)
{
    photobridge::CommandDispatcher dispatcher;
    std::ostringstream out;
    std::ostringstream err;
    photobridge::WorkspaceService workspace_service;
    photobridge::CommandContext context{out, err, workspace_service};

    const auto status = dispatcher.Dispatch("missing", context);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), photobridge::StatusCode::kNotFound);
}

TEST(RunCliTest, ScanCommandUsesInjectedOutput)
{
    std::ostringstream out;
    std::ostringstream err;

    const int exit_code = RunCliWithArgs(
        {"photobridge", "scan"},
        out,
        err);

    EXPECT_EQ(exit_code, 0);
    EXPECT_EQ(out.str(), "scan command\n");
    EXPECT_TRUE(err.str().empty());
}

TEST(RunCliTest, InitCreatesWorkspace)
{
    const auto root = std::filesystem::temp_directory_path()
        / "photobridge_cli_init_test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);

    std::ostringstream out;
    std::ostringstream err;
    const int exit_code = RunCliWithArgs(
        {"photobridge", "init", "--workspace", root.string()},
        out,
        err);

    EXPECT_EQ(exit_code, 0);
    EXPECT_TRUE(std::filesystem::is_directory(root / "manifests"));
    EXPECT_TRUE(std::filesystem::is_regular_file(root / "photobridge.db"));
    EXPECT_NE(out.str().find("initialized workspace:"), std::string::npos);
    EXPECT_TRUE(err.str().empty());

    std::filesystem::remove_all(root, cleanup_error);
}
