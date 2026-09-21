#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <sqlite3.h>
#include <gtest/gtest.h>

#include "photobridge/app/runtime_db_writer.h"
#include "photobridge/app/sqlite_schema.h"

namespace {

using namespace photobridge;

TaskSpec MakeTask(int index)
{
    const std::string id = "task-" + std::to_string(index);
    auto target = RelativePath::Parse(id + ".jpg");
    EXPECT_TRUE(target.ok());
    return TaskSpec{id, "key-" + id, TaskType::kMigrateFile,
                    "asset-" + id, "source-" + id,
                    std::move(target.value()), 12, std::nullopt};
}

class RuntimeDbWriterTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        static std::atomic<int> sequence{0};
        path_ = std::filesystem::temp_directory_path()
            / ("photobridge_runtime_writer_"
               + std::to_string(++sequence) + ".db");
        std::error_code error;
        std::filesystem::remove(path_, error);
        auto opened = SqliteConnection::Open(path_);
        ASSERT_TRUE(opened.ok()) << opened.status().message();
        connection_ = std::move(opened.value());
        ASSERT_TRUE(EnsureSchema(connection_).ok());
        ASSERT_TRUE(connection_.Execute(
            "INSERT INTO migration(migration_id, source_manifest_id, target_root, "
            "state, created_at_ns) VALUES('migration-1', 'manifest-1', X'2F', 0, 1);"
            "INSERT INTO migration_plan(plan_id, migration_id, plan_path, "
            "artifact_digest, semantic_digest, semantic_profile_version, "
            "format_version, state, created_at_ns) VALUES("
            "'plan-1', 'migration-1', X'706C616E', zeroblob(32), zeroblob(32), "
            "1, 1, 0, 1);").ok());
    }

    void TearDown() override
    {
        connection_ = SqliteConnection{};
        std::error_code error;
        std::filesystem::remove(path_, error);
        std::filesystem::remove(path_.string() + "-wal", error);
        std::filesystem::remove(path_.string() + "-shm", error);
    }

    void PrepareTasks(int count)
    {
        TaskRuntimeRepository repository(connection_);
        for (int index = 0; index < count; ++index) {
            auto task = MakeTask(index);
            ASSERT_TRUE(repository.AddTask("plan-1", task).ok());
            ASSERT_TRUE(repository.SetReady("plan-1", task.id).ok());
        }
        auto epoch = repository.AcquireNextExecutionEpoch("plan-1");
        ASSERT_TRUE(epoch.ok());
        ASSERT_EQ(epoch.value().value, 1U);
    }

    int AttemptState(const std::string& attempt_id)
    {
        sqlite3_stmt* statement = nullptr;
        EXPECT_EQ(sqlite3_prepare_v2(connection_.native_handle(),
            "SELECT file_state FROM task_attempt WHERE attempt_id = ?1;",
            -1, &statement, nullptr), SQLITE_OK);
        if (!statement) return -1;
        sqlite3_bind_text(statement, 1, attempt_id.c_str(), -1, SQLITE_TRANSIENT);
        const int step = sqlite3_step(statement);
        EXPECT_EQ(step, SQLITE_ROW);
        const int state = step == SQLITE_ROW
            ? sqlite3_column_int(statement, 0) : -1;
        sqlite3_finalize(statement);
        return state;
    }

    std::filesystem::path path_;
    SqliteConnection connection_;
};

TEST_F(RuntimeDbWriterTest, AckFollowsCommitAndRepositoryErrorsPropagate)
{
    PrepareTasks(1);
    auto opened = RuntimeDbWriter::Start(path_);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto& writer = *opened.value();
    auto claim = writer.ClaimNextReady("plan-1", {1});
    ASSERT_TRUE(claim.ok()) << claim.status().message();
    const auto attempt = *claim.value().runtime.attempt_id;
    EXPECT_EQ(AttemptState(attempt), static_cast<int>(FileAttemptState::kRunning));

    EXPECT_EQ(writer.MarkTempWritten("plan-1", claim.value().id, {1}, attempt)
                  .code(), StatusCode::kInvalidArgument);
    ASSERT_TRUE(writer.MarkCommitIntent("plan-1", claim.value().id,
                                        {1}, attempt).ok());
    EXPECT_EQ(AttemptState(attempt),
              static_cast<int>(FileAttemptState::kCommitIntent));
    ASSERT_TRUE(writer.MarkTempWritten("plan-1", claim.value().id,
                                       {1}, attempt).ok());
    EXPECT_EQ(AttemptState(attempt),
              static_cast<int>(FileAttemptState::kTempWritten));
    EXPECT_EQ(writer.ClaimNextReady("plan-1", {1}).status().code(),
              StatusCode::kNotFound);
    writer.Stop();
    EXPECT_EQ(writer.MarkCommitIntent("plan-1", claim.value().id,
                                     {1}, attempt).code(), StatusCode::kInternal);
}

TEST_F(RuntimeDbWriterTest, MultiThreadSubmitDrainsEveryCommand)
{
    constexpr int kTasks = 64;
    PrepareTasks(kTasks);
    auto opened = RuntimeDbWriter::Start(path_);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto& writer = *opened.value();
    std::atomic<int> completed{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 8; ++thread) {
        threads.emplace_back([&] {
            while (true) {
                auto claim = writer.ClaimNextReady("plan-1", {1});
                if (!claim.ok()) {
                    if (claim.status().code() != StatusCode::kNotFound) ++failures;
                    break;
                }
                const auto& task = claim.value();
                const auto& attempt = *task.runtime.attempt_id;
                if (!writer.MarkCommitIntent("plan-1", task.id, {1}, attempt).ok()
                    || !writer.MarkTempWritten("plan-1", task.id, {1}, attempt).ok()) {
                    ++failures;
                } else {
                    ++completed;
                }
            }
        });
    }
    for (auto& thread : threads) thread.join();
    writer.Stop();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(completed.load(), kTasks);
    TaskRuntimeRepository reader(connection_);
    for (int index = 0; index < kTasks; ++index) {
        const auto task = MakeTask(index);
        auto runtime = reader.ReadRuntime("plan-1", task.id);
        ASSERT_TRUE(runtime.ok());
        ASSERT_TRUE(runtime.value().attempt_id.has_value());
        EXPECT_EQ(AttemptState(*runtime.value().attempt_id),
                  static_cast<int>(FileAttemptState::kTempWritten));
    }
}

TEST_F(RuntimeDbWriterTest, ReceiptAndRetryUseTheWriterConnection)
{
    PrepareTasks(2);
    auto opened = RuntimeDbWriter::Start(path_);
    ASSERT_TRUE(opened.ok()) << opened.status().message();
    auto& writer = *opened.value();
    auto first = writer.ClaimNextReady("plan-1", {1});
    ASSERT_TRUE(first.ok());
    const auto attempt = *first.value().runtime.attempt_id;
    ASSERT_TRUE(writer.MarkCommitIntent("plan-1", first.value().id,
                                        {1}, attempt).ok());
    ASSERT_TRUE(writer.MarkTempWritten("plan-1", first.value().id,
                                       {1}, attempt).ok());
    Digest digest;
    FileIdentity identity;
    identity.size = 12;
    const VerifiedReceipt receipt{
        first.value().id, attempt, {1}, "temp.jpg", "target.jpg",
        12, digest, digest, identity};
    ASSERT_TRUE(writer.PersistVerifiedReceipt("plan-1", receipt).ok());
    TaskRuntimeRepository reader(connection_);
    auto persisted = reader.ReadVerifiedReceipt(
        "plan-1", first.value().id, attempt);
    ASSERT_TRUE(persisted.ok()) << persisted.status().message();
    EXPECT_EQ(persisted.value().temp_path, "temp.jpg");
    EXPECT_EQ(AttemptState(attempt),
              static_cast<int>(FileAttemptState::kVerifiedDurable));

    auto second = writer.ClaimNextReady("plan-1", {1});
    ASSERT_TRUE(second.ok());
    const Status failure(StatusCode::kIoError, "simulated copy failure");
    ASSERT_TRUE(writer.MarkRetryable(
        "plan-1", second.value().id, {1},
        *second.value().runtime.attempt_id, failure).ok());
    auto runtime = reader.ReadRuntime("plan-1", second.value().id);
    ASSERT_TRUE(runtime.ok());
    EXPECT_EQ(runtime.value().state, TaskState::kRetryable);
    writer.Stop();
}

}  // namespace
