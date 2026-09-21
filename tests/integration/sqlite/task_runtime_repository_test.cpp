#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>

#include <sqlite3.h>
#include <unistd.h>
#include <gtest/gtest.h>

#include "photobridge/app/sqlite_schema.h"
#include "photobridge/app/task_runtime_repository.h"

namespace {

photobridge::RelativePath Path(const char* value)
{
    auto result = photobridge::RelativePath::Parse(value);
    EXPECT_TRUE(result.ok());
    return std::move(result.value());
}

photobridge::TaskSpec Task(const char* id)
{
    return photobridge::TaskSpec{
        id,
        std::string("key-") + id,
        photobridge::TaskType::kMigrateFile,
        std::string("asset-") + id,
        std::string("source-") + id,
        Path((std::string("target-") + id + ".jpg").c_str()),
        12,
        std::nullopt,
    };
}

photobridge::VerifiedReceipt Receipt()
{
    photobridge::Digest source_digest;
    source_digest.bytes[0] = std::byte{0x11};
    photobridge::Digest target_digest;
    target_digest.bytes[0] = std::byte{0x22};
    photobridge::FileIdentity source_identity;
    source_identity.device = 3;
    source_identity.inode = 4;
    source_identity.size = 12;
    source_identity.mtime_ns = 5;
    source_identity.ctime_ns = 6;
    source_identity.mount_id = 7;
    return photobridge::VerifiedReceipt{
        "a",
        "attempt-a",
        {7},
        ".photobridge.plan-1.a.attempt-a.pbtmp",
        "target-a.jpg",
        12,
        source_digest,
        target_digest,
        source_identity,
    };
}

bool AdvanceToEpoch(
    photobridge::TaskRuntimeRepository& repository,
    std::uint64_t target)
{
    auto current = repository.ReadCurrentEpoch("plan-1");
    if (!current.ok()) return false;
    while (current.value().value < target) {
        current = repository.AcquireNextExecutionEpoch("plan-1");
        if (!current.ok()) return false;
    }
    return current.value().value == target;
}

class TaskRuntimeRepositoryTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        database_path_ = std::filesystem::temp_directory_path()
            / ("photobridge_task_runtime_" + std::to_string(::getpid())
               + "_" + std::to_string(++sequence_) + ".db");
        std::error_code error;
        std::filesystem::remove(database_path_, error);
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove(database_path_, error);
    }

    void CreatePlan(photobridge::SqliteConnection& connection)
    {
        ASSERT_TRUE(connection.Execute(
            "INSERT INTO migration(migration_id, source_manifest_id, target_root, "
            "state, created_at_ns) VALUES('migration-1', 'manifest-1', X'2F', 0, 1);"
            "INSERT INTO migration_plan(plan_id, migration_id, plan_path, "
            "artifact_digest, semantic_digest, semantic_profile_version, "
            "format_version, state, created_at_ns) VALUES(" 
            "'plan-1', 'migration-1', X'706C616E', zeroblob(32), zeroblob(32), "
            "1, 1, 0, 1);").ok());
    }

    std::filesystem::path database_path_;
    static inline int sequence_ = 0;
};

TEST_F(TaskRuntimeRepositoryTest, ClaimsInStableOrderAndRequiresDependencies)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());

    ASSERT_TRUE(repository.AddTask("plan-1", Task("b")).ok());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.AddDependency("plan-1", "b", "a").ok());
    EXPECT_FALSE(repository.SetReady("plan-1", "b").ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));

    const auto claimed = repository.ClaimNextReady("plan-1", {1}, "attempt-a");
    ASSERT_TRUE(claimed.ok()) << claimed.status().message();
    EXPECT_EQ(claimed.value().id, "a");
    EXPECT_EQ(claimed.value().runtime.state, photobridge::TaskState::kRunning);
    EXPECT_EQ(claimed.value().runtime.owner_epoch.value, 1U);
    EXPECT_EQ(claimed.value().runtime.attempt_count, 1U);
    EXPECT_EQ(
        repository.ClaimNextReady("plan-1", {1}, "attempt-b").status().code(),
        photobridge::StatusCode::kNotFound);
}

TEST_F(TaskRuntimeRepositoryTest, GeneratesAttemptIdsInsideAtomicClaim)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("b")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "b").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));

    const auto first = repository.ClaimNextReady("plan-1", {1});
    ASSERT_TRUE(first.ok()) << first.status().message();
    EXPECT_EQ(first.value().id, "a");
    ASSERT_TRUE(first.value().runtime.attempt_id.has_value());
    EXPECT_EQ(*first.value().runtime.attempt_id, "attempt-plan-1-a-1");
    const auto second = repository.ClaimNextReady("plan-1", {1});
    ASSERT_TRUE(second.ok()) << second.status().message();
    EXPECT_EQ(second.value().id, "b");
    ASSERT_TRUE(second.value().runtime.attempt_id.has_value());
    EXPECT_EQ(*second.value().runtime.attempt_id, "attempt-plan-1-b-1");
    EXPECT_EQ(repository.ClaimNextReady("plan-1", {1}).status().code(),
              photobridge::StatusCode::kNotFound);

    const photobridge::Status error(
        photobridge::StatusCode::kIoError, "retry claim");
    ASSERT_TRUE(repository.MarkRetryable(
        "plan-1", "a", {1}, *first.value().runtime.attempt_id, error).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 2));
    const auto retry = repository.ClaimNextReady("plan-1", {2});
    ASSERT_TRUE(retry.ok()) << retry.status().message();
    EXPECT_EQ(retry.value().id, "a");
    ASSERT_TRUE(retry.value().runtime.attempt_id.has_value());
    EXPECT_EQ(*retry.value().runtime.attempt_id, "attempt-plan-1-a-2");
}

TEST_F(TaskRuntimeRepositoryTest, ReusesStatementsAcrossBatchRollbackAndErrors)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());

    ASSERT_TRUE(connection.value().Execute("BEGIN IMMEDIATE;").ok());
    for (int index = 0; index < 32; ++index) {
        const std::string id = "task-" + std::to_string(index);
        ASSERT_TRUE(repository.AddTask("plan-1", Task(id.c_str())).ok());
        ASSERT_TRUE(repository.SetReady("plan-1", id).ok());
    }
    ASSERT_TRUE(connection.value().Execute("ROLLBACK;").ok());
    EXPECT_EQ(
        repository.ReadRuntime("plan-1", "task-0").status().code(),
        photobridge::StatusCode::kNotFound);

    ASSERT_TRUE(connection.value().Execute("BEGIN IMMEDIATE;").ok());
    for (int index = 0; index < 32; ++index) {
        const std::string id = "task-" + std::to_string(index);
        ASSERT_TRUE(repository.AddTask("plan-1", Task(id.c_str())).ok());
        ASSERT_TRUE(repository.SetReady("plan-1", id).ok());
    }
    ASSERT_TRUE(connection.value().Execute("COMMIT;").ok());

    EXPECT_EQ(
        repository.AddTask("plan-1", Task("task-0")).code(),
        photobridge::StatusCode::kAlreadyExists);
    ASSERT_TRUE(repository.AddTask("plan-1", Task("extra")).ok());
    EXPECT_EQ(
        repository.AddDependency("plan-1", "extra", "missing").code(),
        photobridge::StatusCode::kNotFound);
    ASSERT_TRUE(repository.AddDependency("plan-1", "extra", "task-0").ok());
    EXPECT_EQ(
        repository.SetReady("plan-1", "extra").code(),
        photobridge::StatusCode::kInvalidArgument);
    const auto runtime = repository.ReadRuntime("plan-1", "task-31");
    ASSERT_TRUE(runtime.ok());
    EXPECT_EQ(runtime.value().state, photobridge::TaskState::kReady);
}

TEST_F(TaskRuntimeRepositoryTest, ExecutionEpochRepositoryIsMonotonic)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());

    ASSERT_TRUE(repository.ReadCurrentEpoch("plan-1").ok());
    EXPECT_EQ(repository.ReadCurrentEpoch("plan-1").value().value, 0U);
    EXPECT_EQ(repository.AcquireNextExecutionEpoch("plan-1").value().value, 1U);
    EXPECT_EQ(repository.ReadCurrentEpoch("plan-1").value().value, 1U);
    EXPECT_EQ(repository.AcquireNextExecutionEpoch("plan-1").value().value, 2U);
    EXPECT_EQ(repository.ReadCurrentEpoch("plan-1").value().value, 2U);
    EXPECT_EQ(
        repository.ReadCurrentEpoch("missing-plan").status().code(),
        photobridge::StatusCode::kNotFound);
}

TEST_F(TaskRuntimeRepositoryTest, FencesStaleCompletionAndAllowsRetry)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 4));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {4}, "attempt-a").ok());

    EXPECT_FALSE(
        repository.MarkSucceeded("plan-1", "a", {3}, "attempt-a").ok());
    const photobridge::Status error(
        photobridge::StatusCode::kIoError,
        "temporary I/O failure");
    ASSERT_TRUE(
        repository.MarkRetryable("plan-1", "a", {4}, "attempt-a", error).ok());
    const auto runtime = repository.ReadRuntime("plan-1", "a");
    ASSERT_TRUE(runtime.ok());
    EXPECT_EQ(runtime.value().state, photobridge::TaskState::kRetryable);
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
}

TEST_F(TaskRuntimeRepositoryTest, ClaimRequiresCurrentEpoch)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));

    EXPECT_EQ(
        repository.ClaimNextReady("plan-1", {2}, "attempt-a").status().code(),
        photobridge::StatusCode::kInvalidArgument);
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {1}, "attempt-a").ok());
}

TEST_F(TaskRuntimeRepositoryTest, FinishRequiresCurrentEpoch)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {1}, "attempt-a").ok());

    EXPECT_EQ(
        repository.MarkSucceeded("plan-1", "a", {2}, "attempt-a").code(),
        photobridge::StatusCode::kInvalidArgument);
    ASSERT_TRUE(repository.MarkSucceeded("plan-1", "a", {1}, "attempt-a").ok());
}

TEST_F(TaskRuntimeRepositoryTest, EpochAdvanceFencesOldFinish)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {1}, "attempt-a").ok());
    ASSERT_EQ(repository.AcquireNextExecutionEpoch("plan-1").value().value, 2U);

    EXPECT_EQ(
        repository.MarkSucceeded("plan-1", "a", {1}, "attempt-a").code(),
        photobridge::StatusCode::kInvalidArgument);
}

TEST_F(TaskRuntimeRepositoryTest, StaleEpochCannotPersistReceipt)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {1}, "attempt-a").ok());

    auto receipt = Receipt();
    receipt.owner_epoch = {2};
    EXPECT_EQ(
        repository.PersistVerifiedReceipt("plan-1", receipt).code(),
        photobridge::StatusCode::kInvalidArgument);
}

TEST_F(TaskRuntimeRepositoryTest, WrongAttemptCannotPersistReceipt)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {1}, "attempt-a").ok());

    auto receipt = Receipt();
    receipt.owner_epoch = {1};
    receipt.attempt_id = "attempt-wrong";
    EXPECT_EQ(
        repository.PersistVerifiedReceipt("plan-1", receipt).code(),
        photobridge::StatusCode::kInvalidArgument);
}

TEST_F(TaskRuntimeRepositoryTest, RecoverySuccessClosesOldAttempt)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {1}, "attempt-a").ok());
    ASSERT_EQ(repository.AcquireNextExecutionEpoch("plan-1").value().value, 2U);

    ASSERT_TRUE(repository.RecoverSucceeded(
        "plan-1", "a", {2}, {1}, "attempt-a", "matching final adopted").ok());
    const auto runtime = repository.ReadRuntime("plan-1", "a");
    ASSERT_TRUE(runtime.ok());
    EXPECT_EQ(runtime.value().state, photobridge::TaskState::kSucceeded);
    EXPECT_EQ(runtime.value().owner_epoch.value, 0U);
    EXPECT_FALSE(runtime.value().attempt_id.has_value());

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        connection.value().native_handle(),
        "SELECT file_state, finished_at_ns FROM task_attempt "
        "WHERE attempt_id = 'attempt-a';",
        -1,
        &statement,
        nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(
        sqlite3_column_int(statement, 0),
        static_cast<int>(photobridge::FileAttemptState::kCommitted));
    EXPECT_GT(sqlite3_column_int64(statement, 1), 0);
    sqlite3_finalize(statement);

    ASSERT_EQ(sqlite3_prepare_v2(
        connection.value().native_handle(),
        "SELECT event_type, owner_epoch, detail FROM task_event "
        "WHERE event_type = 'RECOVER_SUCCEEDED';",
        -1,
        &statement,
        nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int64(statement, 1), 2);
    EXPECT_NE(
        std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 2)))
            .find("old_epoch=1"),
        std::string::npos);
    sqlite3_finalize(statement);
}

TEST_F(TaskRuntimeRepositoryTest, RecoveryRetryableRecordsReason)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {1}, "attempt-a").ok());
    ASSERT_EQ(repository.AcquireNextExecutionEpoch("plan-1").value().value, 2U);

    ASSERT_TRUE(repository.RecoverRetryable(
        "plan-1", "a", {2}, {1}, "attempt-a", "source changed").ok());
    const auto runtime = repository.ReadRuntime("plan-1", "a");
    ASSERT_TRUE(runtime.ok());
    EXPECT_EQ(runtime.value().state, photobridge::TaskState::kRetryable);
    ASSERT_TRUE(runtime.value().last_error.has_value());
    EXPECT_EQ(runtime.value().last_error->code(), photobridge::StatusCode::kIoError);
    EXPECT_EQ(runtime.value().last_error->message(), "source changed");
}

TEST_F(TaskRuntimeRepositoryTest, RecoveryInconsistentRecordsTerminalState)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {1}, "attempt-a").ok());
    ASSERT_EQ(repository.AcquireNextExecutionEpoch("plan-1").value().value, 2U);

    ASSERT_TRUE(repository.RecoverInconsistent(
        "plan-1", "a", {2}, {1}, "attempt-a", "target conflict").ok());
    const auto runtime = repository.ReadRuntime("plan-1", "a");
    ASSERT_TRUE(runtime.ok());
    EXPECT_EQ(runtime.value().state, photobridge::TaskState::kInconsistent);
    ASSERT_TRUE(runtime.value().last_error.has_value());
    EXPECT_EQ(runtime.value().last_error->code(), photobridge::StatusCode::kInternal);
}

TEST_F(TaskRuntimeRepositoryTest, RecoveryRequiresCurrentEpochAndOldOwnership)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {1}, "attempt-a").ok());
    ASSERT_EQ(repository.AcquireNextExecutionEpoch("plan-1").value().value, 2U);

    EXPECT_EQ(
        repository.RecoverSucceeded(
            "plan-1", "a", {1}, {1}, "attempt-a", "stale recovery")
            .code(),
        photobridge::StatusCode::kInvalidArgument);
    EXPECT_EQ(
        repository.RecoverSucceeded(
            "plan-1", "a", {2}, {1}, "attempt-wrong", "wrong owner")
            .code(),
        photobridge::StatusCode::kInvalidArgument);
    EXPECT_EQ(
        repository.ReadRuntime("plan-1", "a").value().state,
        photobridge::TaskState::kRunning);
}

TEST_F(TaskRuntimeRepositoryTest, ClaimCreatesRunningAttempt)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {1}, "attempt-a").ok());

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        connection.value().native_handle(),
        "SELECT file_state, started_at_ns FROM task_attempt "
        "WHERE attempt_id = 'attempt-a';",
        -1,
        &statement,
        nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(
        sqlite3_column_int(statement, 0),
        static_cast<int>(photobridge::FileAttemptState::kRunning));
    EXPECT_GT(sqlite3_column_int64(statement, 1), 0);
    sqlite3_finalize(statement);

    ASSERT_EQ(sqlite3_prepare_v2(
        connection.value().native_handle(),
        "SELECT created_at_ns FROM task_event WHERE event_type = 'RUNNING';",
        -1,
        &statement,
        nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_GT(sqlite3_column_int64(statement, 0), 0);
    sqlite3_finalize(statement);
}

TEST_F(TaskRuntimeRepositoryTest, RecordsAttemptCommitLifecycle)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {1}, "attempt-a").ok());

    ASSERT_TRUE(repository.MarkCommitIntent(
        "plan-1", "a", {1}, "attempt-a").ok());
    ASSERT_TRUE(repository.MarkTempWritten(
        "plan-1", "a", {1}, "attempt-a").ok());

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        connection.value().native_handle(),
        "SELECT file_state, finished_at_ns FROM task_attempt "
        "WHERE attempt_id = 'attempt-a';",
        -1,
        &statement,
        nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(
        sqlite3_column_int(statement, 0),
        static_cast<int>(photobridge::FileAttemptState::kTempWritten));
    EXPECT_EQ(sqlite3_column_type(statement, 1), SQLITE_NULL);
    sqlite3_finalize(statement);

    ASSERT_EQ(sqlite3_prepare_v2(
        connection.value().native_handle(),
        "SELECT event_type FROM task_event WHERE attempt_id = 'attempt-a' "
        "ORDER BY event_id;",
        -1,
        &statement,
        nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_STREQ(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)),
        "RUNNING");
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_STREQ(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)),
        "COMMIT_INTENT");
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_STREQ(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)),
        "TEMP_WRITTEN");
    EXPECT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);
}

TEST_F(TaskRuntimeRepositoryTest, RejectsDuplicateTaskKeyAndUnknownDependency)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    EXPECT_EQ(
        repository.AddTask("plan-1", Task("a")).code(),
        photobridge::StatusCode::kAlreadyExists);
    EXPECT_EQ(
        repository.AddDependency("plan-1", "a", "missing").code(),
        photobridge::StatusCode::kNotFound);
}

TEST_F(TaskRuntimeRepositoryTest, DependencyCannotCrossPlanBoundary)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    ASSERT_TRUE(connection.value().Execute(
        "INSERT INTO migration(migration_id, source_manifest_id, target_root, "
        "state, created_at_ns) VALUES('migration-2', 'manifest-2', X'2F', 0, 1);"
        "INSERT INTO migration_plan(plan_id, migration_id, plan_path, "
        "artifact_digest, semantic_digest, semantic_profile_version, "
        "format_version, state, created_at_ns) VALUES(" 
        "'plan-2', 'migration-2', X'706C616E', zeroblob(32), zeroblob(32), "
        "1, 1, 0, 1);").ok());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.AddTask("plan-2", Task("b")).ok());
    EXPECT_EQ(
        repository.AddDependency("plan-1", "a", "b").code(),
        photobridge::StatusCode::kNotFound);
}

TEST_F(TaskRuntimeRepositoryTest, RejectsPersistedDependencyCycle)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("b")).ok());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("c")).ok());
    ASSERT_TRUE(repository.AddDependency("plan-1", "b", "a").ok());
    ASSERT_TRUE(repository.AddDependency("plan-1", "c", "b").ok());
    EXPECT_EQ(
        repository.AddDependency("plan-1", "a", "c").code(),
        photobridge::StatusCode::kInvalidArgument);
}

TEST_F(TaskRuntimeRepositoryTest, ClaimRequiresFencingIdentity)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    EXPECT_EQ(
        repository.ClaimNextReady("plan-1", {0}, "attempt-a").status().code(),
        photobridge::StatusCode::kInvalidArgument);
    EXPECT_EQ(
        repository.ClaimNextReady("plan-1", {1}, "").status().code(),
        photobridge::StatusCode::kInvalidArgument);
}

TEST_F(TaskRuntimeRepositoryTest, DurableEventsTrackClaimAndCompletion)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {1}, "attempt-a").ok());
    ASSERT_TRUE(repository.MarkSucceeded("plan-1", "a", {1}, "attempt-a").ok());

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            connection.value().native_handle(),
            "SELECT event_type, attempt_id FROM task_event "
            "WHERE plan_id = 'plan-1' ORDER BY event_id;",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)), "RUNNING");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)), "attempt-a");
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)), "SUCCEEDED");
    sqlite3_finalize(statement);
}

TEST_F(TaskRuntimeRepositoryTest, SuccessClosesAttemptAsCommitted)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 1));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {1}, "attempt-a").ok());
    ASSERT_TRUE(repository.MarkSucceeded("plan-1", "a", {1}, "attempt-a").ok());

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        connection.value().native_handle(),
        "SELECT file_state, finished_at_ns, result, error_code, error_message "
        "FROM task_attempt WHERE attempt_id = 'attempt-a';",
        -1,
        &statement,
        nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(
        sqlite3_column_int(statement, 0),
        static_cast<int>(photobridge::FileAttemptState::kCommitted));
    EXPECT_GT(sqlite3_column_int64(statement, 1), 0);
    EXPECT_EQ(
        sqlite3_column_int(statement, 2),
        static_cast<int>(photobridge::TaskState::kSucceeded));
    EXPECT_EQ(sqlite3_column_type(statement, 3), SQLITE_NULL);
    EXPECT_EQ(sqlite3_column_type(statement, 4), SQLITE_NULL);
    sqlite3_finalize(statement);
}

TEST_F(TaskRuntimeRepositoryTest, PersistsVerifiedReceiptBeforeCompletion)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 7));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {7}, "attempt-a").ok());

    const photobridge::VerifiedReceipt receipt = Receipt();
    ASSERT_TRUE(repository.PersistVerifiedReceipt("plan-1", receipt).ok());

    const auto loaded = repository.ReadVerifiedReceipt(
        "plan-1", "a", "attempt-a");
    ASSERT_TRUE(loaded.ok()) << loaded.status().message();
    EXPECT_EQ(loaded.value().task_id, receipt.task_id);
    EXPECT_EQ(loaded.value().attempt_id, receipt.attempt_id);
    EXPECT_EQ(loaded.value().owner_epoch, receipt.owner_epoch);
    EXPECT_EQ(loaded.value().temp_path, receipt.temp_path);
    EXPECT_EQ(loaded.value().final_path, receipt.final_path);
    EXPECT_EQ(loaded.value().content_size, receipt.content_size);
    EXPECT_EQ(loaded.value().source_digest, receipt.source_digest);
    EXPECT_EQ(loaded.value().target_digest, receipt.target_digest);
    ASSERT_TRUE(loaded.value().source_identity.has_value());
    EXPECT_EQ(loaded.value().source_identity.value(), receipt.source_identity.value());

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            connection.value().native_handle(),
            "SELECT event_type FROM task_event WHERE plan_id = 'plan-1' "
            "ORDER BY event_id;",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_STREQ(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)),
        "VERIFIED_DURABLE");
    sqlite3_finalize(statement);
}

TEST_F(TaskRuntimeRepositoryTest, VerifiedReceiptMarksAttemptVerifiedDurable)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 7));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {7}, "attempt-a").ok());
    ASSERT_TRUE(repository.PersistVerifiedReceipt("plan-1", Receipt()).ok());

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        connection.value().native_handle(),
        "SELECT file_state, finished_at_ns FROM task_attempt "
        "WHERE attempt_id = 'attempt-a';",
        -1,
        &statement,
        nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(
        sqlite3_column_int(statement, 0),
        static_cast<int>(photobridge::FileAttemptState::kVerifiedDurable));
    EXPECT_EQ(sqlite3_column_type(statement, 1), SQLITE_NULL);
    sqlite3_finalize(statement);
}

TEST_F(TaskRuntimeRepositoryTest, RetryMarksAttemptRetryable)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 7));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {7}, "attempt-a").ok());
    const photobridge::Status error(
        photobridge::StatusCode::kIoError, "temporary failure");
    ASSERT_TRUE(repository.MarkRetryable(
        "plan-1", "a", {7}, "attempt-a", error).ok());

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        connection.value().native_handle(),
        "SELECT file_state, finished_at_ns, result, error_code, error_message "
        "FROM task_attempt WHERE attempt_id = 'attempt-a';",
        -1,
        &statement,
        nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(
        sqlite3_column_int(statement, 0),
        static_cast<int>(photobridge::FileAttemptState::kRetryable));
    EXPECT_GT(sqlite3_column_int64(statement, 1), 0);
    EXPECT_EQ(
        sqlite3_column_int(statement, 2),
        static_cast<int>(photobridge::TaskState::kRetryable));
    EXPECT_EQ(
        sqlite3_column_int(statement, 3),
        static_cast<int>(photobridge::StatusCode::kIoError));
    EXPECT_STREQ(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 4)),
        "temporary failure");
    sqlite3_finalize(statement);
}

TEST_F(TaskRuntimeRepositoryTest, SameReceiptReplayIsIdempotent)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 7));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {7}, "attempt-a").ok());

    const auto receipt = Receipt();
    ASSERT_TRUE(repository.PersistVerifiedReceipt("plan-1", receipt).ok());
    ASSERT_TRUE(repository.PersistVerifiedReceipt("plan-1", receipt).ok());

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(
        connection.value().native_handle(),
        "SELECT COUNT(*) FROM task_event WHERE plan_id = 'plan-1' "
        "AND event_type = 'VERIFIED_DURABLE';",
        -1,
        &statement,
        nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0), 1);
    sqlite3_finalize(statement);
}

TEST_F(TaskRuntimeRepositoryTest, ConflictingReceiptDigestFails)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 7));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {7}, "attempt-a").ok());

    const auto receipt = Receipt();
    ASSERT_TRUE(repository.PersistVerifiedReceipt("plan-1", receipt).ok());
    auto conflicting = receipt;
    conflicting.target_digest.bytes[0] = std::byte{0x33};
    EXPECT_EQ(
        repository.PersistVerifiedReceipt("plan-1", conflicting).code(),
        photobridge::StatusCode::kInternal);
}

TEST_F(TaskRuntimeRepositoryTest, ConflictingReceiptPathFails)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 7));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {7}, "attempt-a").ok());

    const auto receipt = Receipt();
    ASSERT_TRUE(repository.PersistVerifiedReceipt("plan-1", receipt).ok());
    auto conflicting = receipt;
    conflicting.final_path = "other-target.jpg";
    EXPECT_EQ(
        repository.PersistVerifiedReceipt("plan-1", conflicting).code(),
        photobridge::StatusCode::kInternal);
}

TEST_F(TaskRuntimeRepositoryTest, ConflictingReceiptIdentityFails)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 7));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {7}, "attempt-a").ok());

    const auto receipt = Receipt();
    ASSERT_TRUE(repository.PersistVerifiedReceipt("plan-1", receipt).ok());
    auto conflicting = receipt;
    conflicting.source_identity->inode = 99;
    EXPECT_EQ(
        repository.PersistVerifiedReceipt("plan-1", conflicting).code(),
        photobridge::StatusCode::kInternal);
}

TEST_F(TaskRuntimeRepositoryTest, ReceiptReadRejectsNegativeEpoch)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 7));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {7}, "attempt-a").ok());
    ASSERT_TRUE(repository.PersistVerifiedReceipt("plan-1", Receipt()).ok());
    ASSERT_TRUE(connection.value().Execute(
        "UPDATE verified_receipt SET owner_epoch = -1;").ok());

    EXPECT_EQ(
        repository.ReadVerifiedReceipt("plan-1", "a", "attempt-a").status().code(),
        photobridge::StatusCode::kInternal);
}

TEST_F(TaskRuntimeRepositoryTest, ReceiptReadRejectsNegativeSize)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(repository, 7));
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {7}, "attempt-a").ok());
    ASSERT_TRUE(repository.PersistVerifiedReceipt("plan-1", Receipt()).ok());
    ASSERT_TRUE(connection.value().Execute(
        "PRAGMA ignore_check_constraints = ON;"
        "UPDATE verified_receipt SET content_size = -1;").ok());

    EXPECT_EQ(
        repository.ReadVerifiedReceipt("plan-1", "a", "attempt-a").status().code(),
        photobridge::StatusCode::kInternal);
}

TEST_F(TaskRuntimeRepositoryTest, MultipleConnectionsCannotClaimSameReadyTask)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository first(connection.value());
    ASSERT_TRUE(first.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(first.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(AdvanceToEpoch(first, 1));
    
    auto second_connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(second_connection.ok());
    photobridge::TaskRuntimeRepository second(second_connection.value());
    ASSERT_TRUE(first.ClaimNextReady("plan-1", {1}, "attempt-a").ok());
    EXPECT_EQ(
        second.ClaimNextReady("plan-1", {1}, "attempt-b").status().code(),
        photobridge::StatusCode::kNotFound);
}

}  // namespace
