#include <algorithm>
#include <memory>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>


#include <sqlite3.h>
#include <gtest/gtest.h>

#include "photobridge/app/sqlite_connection.h"
#include "photobridge/cli/cli_app.h"
#include "photobridge/cli/dispatcher.h"
#include "photobridge/cli/pipeline_command.h"
#include "photobridge/app/workspace_service.h"
#include "photobridge/model/plan_artifact.h"

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

TEST(RunCliTest, ScanPipelineUsesInjectedOutput)
{
    const auto root = std::filesystem::temp_directory_path()
        / "photobridge_cli_scan_test";
    const auto workspace = std::filesystem::temp_directory_path()
        / "photobridge_cli_scan_workspace";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::remove_all(workspace, cleanup_error);
    ASSERT_TRUE(std::filesystem::create_directories(root, cleanup_error));
    ASSERT_FALSE(cleanup_error);
    {
        std::ofstream file(root / "photo.jpg");
        ASSERT_TRUE(file);
        file << "photo";
    }

    std::ostringstream init_out;
    std::ostringstream init_err;
    ASSERT_EQ(
        RunCliWithArgs(
            {"photobridge", "init", "--workspace", workspace.string()},
            init_out,
            init_err),
        0);

    std::ostringstream out;
    std::ostringstream err;

    const int exit_code = RunCliWithArgs(
        {"photobridge", "scan", "--workspace", workspace.string(),
            "--source", root.string()},
        out,
        err);

    EXPECT_EQ(exit_code, 0);
    EXPECT_NE(out.str().find("scan completed:"), std::string::npos);
    EXPECT_NE(out.str().find("assets=1"), std::string::npos);
    EXPECT_TRUE(err.str().empty());

    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::remove_all(workspace, cleanup_error);
}

TEST(RunCliTest, PlanBuildsAndPersistsFrozenArtifact)
{
    const auto source = std::filesystem::temp_directory_path()
        / "photobridge_cli_plan_source";
    const auto workspace = std::filesystem::temp_directory_path()
        / "photobridge_cli_plan_workspace";
    const auto target = std::filesystem::temp_directory_path()
        / "photobridge_cli_plan_target";
    std::error_code cleanup_error;
    std::filesystem::remove_all(source, cleanup_error);
    std::filesystem::remove_all(workspace, cleanup_error);
    std::filesystem::remove_all(target, cleanup_error);
    ASSERT_TRUE(std::filesystem::create_directories(source, cleanup_error));
    ASSERT_FALSE(cleanup_error);
    {
        std::ofstream file(source / "photo.jpg");
        ASSERT_TRUE(file);
        file << "photo";
    }

    std::ostringstream init_out;
    std::ostringstream init_err;
    ASSERT_EQ(
        RunCliWithArgs(
            {"photobridge", "init", "--workspace", workspace.string()},
            init_out,
            init_err),
        0);

    std::ostringstream scan_out;
    std::ostringstream scan_err;
    ASSERT_EQ(
        RunCliWithArgs(
            {"photobridge", "scan", "--workspace", workspace.string(),
                "--source", source.string()},
            scan_out,
            scan_err),
        0);
    const std::string scan_prefix = "scan completed: ";
    const std::size_t manifest_start = scan_out.str().find(scan_prefix);
    ASSERT_NE(manifest_start, std::string::npos);
    const std::size_t id_start = manifest_start + scan_prefix.size();
    const std::size_t id_end = scan_out.str().find(' ', id_start);
    ASSERT_NE(id_end, std::string::npos);
    const std::string manifest_id = scan_out.str().substr(
        id_start,
        id_end - id_start);

    std::ostringstream plan_out;
    std::ostringstream plan_err;
    ASSERT_EQ(
        RunCliWithArgs(
            {"photobridge", "plan", "--workspace", workspace.string(),
                "--manifest", manifest_id, "--target", target.string()},
            plan_out,
            plan_err),
        0);
    EXPECT_NE(plan_out.str().find("plan completed:"), std::string::npos);
    EXPECT_NE(plan_out.str().find("assets=1"), std::string::npos);
    EXPECT_TRUE(plan_err.str().empty());

    std::size_t artifact_count = 0;
    std::filesystem::path plan_artifact_path;
    for (const auto& entry : std::filesystem::directory_iterator(
             workspace / "plans")) {
        if (entry.path().extension() != ".jsonl") {
            continue;
        }
        ++artifact_count;
        plan_artifact_path = entry.path();
        std::ifstream file(entry.path(), std::ios::binary);
        ASSERT_TRUE(file);
        const std::string bytes{
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
        const auto frozen = photobridge::ReadFrozenPlan(bytes);
        ASSERT_TRUE(frozen.ok()) << frozen.status().message();
        EXPECT_EQ(frozen.value().plan.source_manifest_id(), manifest_id);
        EXPECT_EQ(frozen.value().plan.target_root(), target.string());
        EXPECT_EQ(frozen.value().plan.assets().size(), 1U);
    }
    EXPECT_EQ(artifact_count, 1U);

    std::ostringstream migrate_out;
    std::ostringstream migrate_err;
    ASSERT_EQ(
        RunCliWithArgs(
            {"photobridge", "migrate", "--workspace", workspace.string(),
                "--plan", plan_artifact_path.string()},
            migrate_out,
            migrate_err),
        0);
    EXPECT_NE(migrate_out.str().find("migrate claimed:"), std::string::npos);
    EXPECT_NE(migrate_out.str().find("tasks=1"), std::string::npos);
    EXPECT_NE(
        migrate_out.str().find("source_bytes=5"),
        std::string::npos);
    EXPECT_NE(
        migrate_out.str().find("state=RUNNING"),
        std::string::npos);
    std::size_t temp_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(target)) {
        if (entry.path().extension() == ".pbtmp") {
            ++temp_count;
        }
    }
    EXPECT_EQ(temp_count, 1U);
    EXPECT_TRUE(migrate_err.str().empty());

    std::ostringstream resume_out;
    std::ostringstream resume_err;
    ASSERT_EQ(
        RunCliWithArgs(
            {"photobridge", "resume", "--workspace", workspace.string(),
                "--plan", plan_artifact_path.string()},
            resume_out,
            resume_err),
        0) << "stdout=" << resume_out.str() << " stderr=" << resume_err.str();
    EXPECT_NE(resume_out.str().find("resume completed:"), std::string::npos);
    EXPECT_NE(resume_out.str().find("state=SUCCEEDED"), std::string::npos);
    EXPECT_TRUE(resume_err.str().empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(target / "photo.jpg"));
    temp_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(target)) {
        if (entry.path().extension() == ".pbtmp") {
            ++temp_count;
        }
    }
    EXPECT_EQ(temp_count, 0U);

    std::ostringstream status_out;
    std::ostringstream status_err;
    ASSERT_EQ(
        RunCliWithArgs(
            {"photobridge", "status", "--workspace", workspace.string(),
                "--plan", plan_artifact_path.string()},
            status_out,
            status_err),
        0);
    EXPECT_NE(status_out.str().find("status:"), std::string::npos);
    EXPECT_NE(
        status_out.str().find("tasks=1 SUCCEEDED=1"),
        std::string::npos);
    EXPECT_TRUE(status_err.str().empty());

    std::ostringstream verify_out;
    std::ostringstream verify_err;
    ASSERT_EQ(
        RunCliWithArgs(
            {"photobridge", "verify", "--workspace", workspace.string(),
                "--plan", plan_artifact_path.string()},
            verify_out,
            verify_err),
        0);
    EXPECT_NE(verify_out.str().find("verify completed:"), std::string::npos);
    EXPECT_NE(verify_out.str().find("status=IDENTICAL bytes=5"), std::string::npos);
    EXPECT_TRUE(verify_err.str().empty());

    {
        std::ofstream changed(source / "photo.jpg", std::ios::binary);
        ASSERT_TRUE(changed);
        changed << "changed";
    }
    std::ostringstream changed_verify_out;
    std::ostringstream changed_verify_err;
    EXPECT_EQ(
        RunCliWithArgs(
            {"photobridge", "verify", "--workspace", workspace.string(),
                "--plan", plan_artifact_path.string()},
            changed_verify_out,
            changed_verify_err),
        7);
    EXPECT_TRUE(changed_verify_out.str().empty());
    EXPECT_TRUE(changed_verify_err.str().empty());

    std::ostringstream repeated_out;
    std::ostringstream repeated_err;
    EXPECT_EQ(
        RunCliWithArgs(
            {"photobridge", "migrate", "--workspace", workspace.string(),
                "--plan", plan_artifact_path.string()},
            repeated_out,
            repeated_err),
        3);
    EXPECT_TRUE(repeated_out.str().empty());
    EXPECT_TRUE(repeated_err.str().empty());

    auto connection = photobridge::SqliteConnection::Open(
        workspace / "photobridge.db");
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            connection.value().native_handle(),
            "SELECT COUNT(*) FROM plan_task;",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int64(statement, 0), 1);
    EXPECT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);

    std::filesystem::remove_all(source, cleanup_error);
    std::filesystem::remove_all(workspace, cleanup_error);
    std::filesystem::remove_all(target, cleanup_error);
}

TEST(RunCliTest, MigratesAndResumesMoreTasksThanQueueCapacity)
{
    const auto source = std::filesystem::temp_directory_path()
        / "photobridge_cli_multi_source";
    const auto workspace = std::filesystem::temp_directory_path()
        / "photobridge_cli_multi_workspace";
    const auto target = std::filesystem::temp_directory_path()
        / "photobridge_cli_multi_target";
    std::error_code cleanup_error;
    std::filesystem::remove_all(source, cleanup_error);
    std::filesystem::remove_all(workspace, cleanup_error);
    std::filesystem::remove_all(target, cleanup_error);
    ASSERT_TRUE(std::filesystem::create_directories(source, cleanup_error));
    ASSERT_FALSE(cleanup_error);
    {
        std::ofstream first(source / "a.jpg", std::ios::binary);
        std::ofstream second(source / "b.jpg", std::ios::binary);
        ASSERT_TRUE(first && second);
        first << "one";
        second << "two!";
    }
    for (int index = 0; index < 10; ++index) {
        std::ofstream extra(source / ("extra-" + std::to_string(index) + ".jpg"),
                            std::ios::binary);
        ASSERT_TRUE(extra);
        extra << "x";
    }

    std::ostringstream out;
    std::ostringstream err;
    ASSERT_EQ(
        RunCliWithArgs(
            {"photobridge", "init", "--workspace", workspace.string()},
            out,
            err),
        0);
    out.str("");
    out.clear();
    err.str("");
    err.clear();
    ASSERT_EQ(
        RunCliWithArgs(
            {"photobridge", "scan", "--workspace", workspace.string(),
                "--source", source.string()},
            out,
            err),
        0);
    const std::string scan_prefix = "scan completed: ";
    const std::size_t id_start = out.str().find(scan_prefix) + scan_prefix.size();
    const std::string manifest_id = out.str().substr(
        id_start,
        out.str().find(' ', id_start) - id_start);
    out.str("");
    out.clear();
    err.str("");
    err.clear();
    ASSERT_EQ(
        RunCliWithArgs(
            {"photobridge", "plan", "--workspace", workspace.string(),
                "--manifest", manifest_id, "--target", target.string()},
            out,
            err),
        0);

    std::filesystem::path plan_path;
    for (const auto& entry : std::filesystem::directory_iterator(
             workspace / "plans")) {
        if (entry.path().extension() == ".jsonl") plan_path = entry.path();
    }
    ASSERT_FALSE(plan_path.empty());
    {
        std::ifstream plan_file(plan_path, std::ios::binary);
        const std::string bytes{
            std::istreambuf_iterator<char>(plan_file),
            std::istreambuf_iterator<char>()};
        const auto artifact = photobridge::ReadFrozenPlan(bytes);
        ASSERT_TRUE(artifact.ok()) << artifact.status().message();
        EXPECT_EQ(artifact.value().plan.assets().size(), 12U);
    }

    out.str("");
    out.clear();
    err.str("");
    ASSERT_EQ(
        RunCliWithArgs(
            {"photobridge", "migrate", "--workspace", workspace.string(),
             "--plan", plan_path.string(), "--workers", "2",
             "--db-batch-size", "4"}, out, err),
        0);
    const std::string migrated_output = out.str();
    EXPECT_EQ(std::count(migrated_output.begin(), migrated_output.end(), '\n'), 12);
    out.str("");
    out.clear();
    err.str("");
    err.clear();
    EXPECT_EQ(
        RunCliWithArgs(
            {"photobridge", "migrate", "--workspace", workspace.string(),
             "--plan", plan_path.string()}, out, err),
        2);
    out.str("");
    out.clear();
    err.str("");
    err.clear();
    ASSERT_EQ(
        RunCliWithArgs(
            {"photobridge", "resume", "--workspace", workspace.string(),
             "--plan", plan_path.string()}, out, err),
        0);
    const std::string resumed_output = out.str();
    std::size_t completed = 0;
    std::size_t position = 0;
    while ((position = resumed_output.find("resume completed:", position))
           != std::string::npos) {
        ++completed;
        position += sizeof("resume completed:") - 1;
    }
    EXPECT_EQ(completed, 12U);

    out.str("");
    out.clear();
    err.str("");
    ASSERT_EQ(
        RunCliWithArgs(
            {"photobridge", "verify", "--workspace", workspace.string(),
                "--plan", plan_path.string()},
            out,
            err),
        0);
    EXPECT_NE(out.str().find("tasks=12 status=IDENTICAL bytes=17"), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_regular_file(target / "a.jpg"));
    EXPECT_TRUE(std::filesystem::is_regular_file(target / "b.jpg"));

    std::filesystem::remove_all(source, cleanup_error);
    std::filesystem::remove_all(workspace, cleanup_error);
    std::filesystem::remove_all(target, cleanup_error);
}

TEST(PipelineCommandRoutingTest, DelegatesToPipelineStageValidation)
{
    photobridge::PipelineCommand command(
        photobridge::PipelineStage::kPlan,
        "workspace",
        "");
    std::ostringstream out;
    std::ostringstream err;
    photobridge::WorkspaceService workspace_service;
    photobridge::CommandContext context{out, err, workspace_service};

    const auto status = command.Execute(context);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), photobridge::StatusCode::kInvalidArgument);
    EXPECT_TRUE(out.str().empty());
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
