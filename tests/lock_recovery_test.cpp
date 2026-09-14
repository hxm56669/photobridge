#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <sqlite3.h>

#include <gtest/gtest.h>

#include "photobridge/app/plan_execution_lock.h"
#include "photobridge/app/sqlite_connection.h"

namespace {

int RunPhotobridge(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments)
{
    const pid_t child = ::fork();
    if (child == -1) return 127;
    if (child == 0) {
        const int null_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null_fd == -1) _exit(127);
        ::dup2(null_fd, STDOUT_FILENO);
        ::dup2(null_fd, STDERR_FILENO);
        ::close(null_fd);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(executable.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    if (::waitpid(child, &status, 0) != child) return 127;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

pid_t StartPhotobridge(
    const std::filesystem::path& executable,
    const std::vector<std::string>& arguments)
{
    const pid_t child = ::fork();
    if (child == -1) return -1;
    if (child == 0) {
        const int null_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null_fd == -1) _exit(127);
        ::dup2(null_fd, STDOUT_FILENO);
        ::dup2(null_fd, STDERR_FILENO);
        ::close(null_fd);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(executable.c_str(), argv.data());
        _exit(127);
    }
    return child;
}

bool HasVerifiedReceipt(const std::filesystem::path& workspace)
{
    auto connection = photobridge::SqliteConnection::Open(
        workspace / "photobridge.db");
    if (!connection.ok()) return false;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            connection.value().native_handle(),
            "SELECT COUNT(*) FROM verified_receipt;",
            -1,
            &statement,
            nullptr)
        != SQLITE_OK) {
        return false;
    }
    const int step = sqlite3_step(statement);
    const bool result = step == SQLITE_ROW
        && sqlite3_column_int64(statement, 0) > 0;
    sqlite3_finalize(statement);
    return result;
}

std::filesystem::path FindTemp(const std::filesystem::path& target)
{
    std::error_code error;
    if (!std::filesystem::exists(target, error)) return {};
    for (const auto& entry : std::filesystem::directory_iterator(target, error)) {
        if (!error && entry.path().extension() == ".pbtmp") {
            return entry.path();
        }
    }
    return {};
}

bool WaitUntil(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = std::chrono::seconds(5))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

void KillAndReap(pid_t child)
{
    ASSERT_NE(child, -1);
    ASSERT_EQ(::kill(child, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGKILL);
}

struct CrashScenario {
    std::filesystem::path root;
    std::filesystem::path source;
    std::filesystem::path workspace;
    std::filesystem::path target;
    std::filesystem::path plan;
};

void PrepareCrashScenario(
    const std::filesystem::path& executable,
    int index,
    CrashScenario& result)
{
    const auto base = std::filesystem::temp_directory_path()
        / ("photobridge_process_crash_" + std::to_string(getpid())
           + "_" + std::to_string(index));
    CrashScenario scenario{
        base,
        base / "source",
        base / "workspace",
        base / "target",
        {},
    };
    std::error_code error;
    std::filesystem::remove_all(scenario.root, error);
    ASSERT_TRUE(std::filesystem::create_directories(scenario.source, error));
    ASSERT_FALSE(error);
    std::ofstream source_file(scenario.source / "photo.jpg", std::ios::binary);
    ASSERT_TRUE(source_file);
    std::string block(1024 * 1024, 'p');
    for (int block_index = 0; block_index < 8; ++block_index) {
        source_file.write(block.data(), block.size());
    }
    source_file.close();

    ASSERT_EQ(
        RunPhotobridge(
            executable,
            {"init", "--workspace", scenario.workspace.string()}),
        0);
    const int scan_status = RunPhotobridge(
        executable,
        {"scan", "--workspace", scenario.workspace.string(),
         "--source", scenario.source.string()});
    ASSERT_EQ(scan_status, 0);

    std::string manifest_id;
    auto connection = photobridge::SqliteConnection::Open(
        scenario.workspace / "photobridge.db");
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            connection.value().native_handle(),
            "SELECT manifest_id FROM source_manifest ORDER BY rowid DESC LIMIT 1;",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    manifest_id = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    sqlite3_finalize(statement);
    ASSERT_FALSE(manifest_id.empty());

    ASSERT_EQ(
        RunPhotobridge(
            executable,
            {"plan", "--workspace", scenario.workspace.string(),
             "--manifest", manifest_id, "--target", scenario.target.string()}),
        0);
    for (const auto& entry : std::filesystem::directory_iterator(
             scenario.workspace / "plans")) {
        if (entry.path().extension() == ".jsonl") {
            scenario.plan = entry.path();
            break;
        }
    }
    EXPECT_FALSE(scenario.plan.empty());
    result = scenario;
}

void FinishWithResumeAndVerify(
    const std::filesystem::path& executable,
    const CrashScenario& scenario,
    const char* label)
{
    const int resume_status = RunPhotobridge(
        executable,
        {"resume", "--workspace", scenario.workspace.string(),
         "--plan", scenario.plan.string()});
    EXPECT_EQ(resume_status, 0) << label;
    const int verify_status = RunPhotobridge(
        executable,
        {"verify", "--workspace", scenario.workspace.string(),
         "--plan", scenario.plan.string()});
    EXPECT_EQ(verify_status, 0) << label;
}


}  // namespace

TEST(LockRecoveryTest, KilledOwnerReleasesStableLockForNextOwner)
{
    const auto path = std::filesystem::temp_directory_path()
        / ("photobridge_lock_recovery_" + std::to_string(getpid()));
    const int parent_fd = ::open(
        path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    ASSERT_NE(parent_fd, -1);

    int ready_pipe[2] = {-1, -1};
    ASSERT_EQ(::pipe(ready_pipe), 0);
    const pid_t child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
        ::close(ready_pipe[0]);
        const int child_fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (child_fd == -1 || ::flock(child_fd, LOCK_EX) != 0) {
            _exit(2);
        }
        const char ready = '1';
        if (::write(ready_pipe[1], &ready, 1) != 1) _exit(3);
        for (;;) ::pause();
    }

    ::close(ready_pipe[1]);
    char ready = 0;
    ASSERT_EQ(::read(ready_pipe[0], &ready, 1), 1);
    ASSERT_EQ(::kill(child, SIGKILL), 0);
    int wait_status = 0;
    ASSERT_EQ(::waitpid(child, &wait_status, 0), child);
    EXPECT_TRUE(WIFSIGNALED(wait_status));

    const int next_fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    ASSERT_NE(next_fd, -1);
    EXPECT_EQ(::flock(next_fd, LOCK_EX | LOCK_NB), 0);
    ::close(next_fd);
    ::close(parent_fd);
    ::close(ready_pipe[0]);
    std::error_code error;
    std::filesystem::remove(path, error);
    EXPECT_FALSE(error);
}

TEST(LockRecoveryTest, PlanExecutionLockRejectsSecondExecutor)
{
    const auto path = std::filesystem::temp_directory_path()
        / ("photobridge_plan_execution_lock_" + std::to_string(getpid()));
    auto first = photobridge::PlanExecutionLock::Acquire(path);
    ASSERT_TRUE(first.ok()) << first.status().message();
    const auto second = photobridge::PlanExecutionLock::Acquire(path);
    ASSERT_FALSE(second.ok());
    EXPECT_EQ(second.status().code(), photobridge::StatusCode::kAlreadyExists);
    std::error_code error;
    std::filesystem::remove(path, error);
}
TEST(ProcessCrashE2ETest, SigkillResumeVerifyCoversAllCommitWindows)
{
    const auto self = std::filesystem::read_symlink("/proc/self/exe");
    const auto executable = self.parent_path() / "photobridge";
    ASSERT_TRUE(std::filesystem::is_regular_file(executable));

    {
        CrashScenario scenario{};
        PrepareCrashScenario(executable, 1, scenario);
        ASSERT_FALSE(scenario.plan.empty());
        ASSERT_EQ(
            ::setenv(
                "PHOTOBRIDGE_TEST_PAUSE_AFTER_FIRST_COPY_WRITE_MS",
                "10000",
                1),
            0);
        const pid_t child = StartPhotobridge(
            executable,
            {"migrate", "--workspace", scenario.workspace.string(),
             "--plan", scenario.plan.string()});
        const bool partial_temp = WaitUntil([&scenario] {
            const auto temp = FindTemp(scenario.target);
            std::error_code error;
            return !temp.empty()
                && std::filesystem::file_size(temp, error) > 0
                && !error;
        });
        EXPECT_TRUE(partial_temp);
        EXPECT_FALSE(HasVerifiedReceipt(scenario.workspace));
        KillAndReap(child);
        ::unsetenv("PHOTOBRIDGE_TEST_PAUSE_AFTER_FIRST_COPY_WRITE_MS");

        EXPECT_EQ(
            RunPhotobridge(
                executable,
                {"resume", "--workspace", scenario.workspace.string(),
                 "--plan", scenario.plan.string()}),
            6);
        EXPECT_EQ(
            RunPhotobridge(
                executable,
                {"migrate", "--workspace", scenario.workspace.string(),
                 "--plan", scenario.plan.string()}),
            0);
        FinishWithResumeAndVerify(executable, scenario, "partial temp/no receipt");
        std::error_code error;
        std::filesystem::remove_all(scenario.root, error);
    }

    {
        CrashScenario scenario{};
        PrepareCrashScenario(executable, 2, scenario);
        ASSERT_FALSE(scenario.plan.empty());
        ASSERT_EQ(
            ::setenv("PHOTOBRIDGE_TEST_PAUSE_AFTER_RECEIPT_MS", "10000", 1),
            0);
        const pid_t child = StartPhotobridge(
            executable,
            {"migrate", "--workspace", scenario.workspace.string(),
             "--plan", scenario.plan.string()});
        EXPECT_TRUE(WaitUntil([&scenario] {
            return HasVerifiedReceipt(scenario.workspace)
                && !FindTemp(scenario.target).empty();
        }));
        KillAndReap(child);
        ::unsetenv("PHOTOBRIDGE_TEST_PAUSE_AFTER_RECEIPT_MS");
        FinishWithResumeAndVerify(executable, scenario, "verified receipt/temp");
        std::error_code error;
        std::filesystem::remove_all(scenario.root, error);
    }

    {
        CrashScenario scenario{};
        PrepareCrashScenario(executable, 3, scenario);
        ASSERT_FALSE(scenario.plan.empty());
        ASSERT_EQ(
            RunPhotobridge(
                executable,
                {"migrate", "--workspace", scenario.workspace.string(),
                 "--plan", scenario.plan.string()}),
            0);
        ASSERT_EQ(
            ::setenv("PHOTOBRIDGE_TEST_PAUSE_AFTER_RENAME_MS", "10000", 1),
            0);
        const pid_t child = StartPhotobridge(
            executable,
            {"resume", "--workspace", scenario.workspace.string(),
             "--plan", scenario.plan.string()});
        EXPECT_TRUE(WaitUntil([&scenario] {
            return std::filesystem::is_regular_file(
                       scenario.target / "photo.jpg")
                && FindTemp(scenario.target).empty();
        }));
        KillAndReap(child);
        ::unsetenv("PHOTOBRIDGE_TEST_PAUSE_AFTER_RENAME_MS");
        FinishWithResumeAndVerify(executable, scenario, "rename completed/db running");
        std::error_code error;
        std::filesystem::remove_all(scenario.root, error);
    }

    {
        CrashScenario scenario{};
        PrepareCrashScenario(executable, 4, scenario);
        ASSERT_FALSE(scenario.plan.empty());
        ASSERT_EQ(
            RunPhotobridge(
                executable,
                {"migrate", "--workspace", scenario.workspace.string(),
                 "--plan", scenario.plan.string()}),
            0);
        ASSERT_EQ(
            ::setenv("PHOTOBRIDGE_TEST_PAUSE_AFTER_RENAME_MS", "10000", 1),
            0);
        const pid_t rename_child = StartPhotobridge(
            executable,
            {"resume", "--workspace", scenario.workspace.string(),
             "--plan", scenario.plan.string()});
        EXPECT_TRUE(WaitUntil([&scenario] {
            return std::filesystem::is_regular_file(
                       scenario.target / "photo.jpg")
                && FindTemp(scenario.target).empty();
        }));
        KillAndReap(rename_child);
        ::unsetenv("PHOTOBRIDGE_TEST_PAUSE_AFTER_RENAME_MS");
        ASSERT_EQ(
            ::setenv("PHOTOBRIDGE_TEST_PAUSE_BEFORE_RECOVERY_MS", "10000", 1),
            0);
        const pid_t child = StartPhotobridge(
            executable,
            {"resume", "--workspace", scenario.workspace.string(),
             "--plan", scenario.plan.string()});
        EXPECT_TRUE(WaitUntil([&scenario] {
            return std::filesystem::is_regular_file(
                       scenario.target / "photo.jpg")
                && FindTemp(scenario.target).empty();
        }));
        KillAndReap(child);
        ::unsetenv("PHOTOBRIDGE_TEST_PAUSE_BEFORE_RECOVERY_MS");
        FinishWithResumeAndVerify(executable, scenario, "final durable/before recovery");
        std::error_code error;
        std::filesystem::remove_all(scenario.root, error);
    }
}
