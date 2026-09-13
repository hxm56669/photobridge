#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>

#include <sqlite3.h>
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

class TaskRuntimeRepositoryTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        database_path_ = std::filesystem::temp_directory_path()
            / ("photobridge_task_runtime_" + std::to_string(++sequence_) + ".db");
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

TEST_F(TaskRuntimeRepositoryTest, FencesStaleCompletionAndAllowsRetry)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
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

TEST_F(TaskRuntimeRepositoryTest, PersistsVerifiedReceiptBeforeCompletion)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository repository(connection.value());
    ASSERT_TRUE(repository.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(repository.SetReady("plan-1", "a").ok());
    ASSERT_TRUE(repository.ClaimNextReady("plan-1", {7}, "attempt-a").ok());

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
    const photobridge::VerifiedReceipt receipt{
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
    EXPECT_EQ(loaded.value().source_identity.value(), source_identity);

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

TEST_F(TaskRuntimeRepositoryTest, MultipleConnectionsCannotClaimSameReadyTask)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    CreatePlan(connection.value());
    photobridge::TaskRuntimeRepository first(connection.value());
    ASSERT_TRUE(first.AddTask("plan-1", Task("a")).ok());
    ASSERT_TRUE(first.SetReady("plan-1", "a").ok());

    auto second_connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(second_connection.ok());
    photobridge::TaskRuntimeRepository second(second_connection.value());
    ASSERT_TRUE(first.ClaimNextReady("plan-1", {1}, "attempt-a").ok());
    EXPECT_EQ(
        second.ClaimNextReady("plan-1", {2}, "attempt-b").status().code(),
        photobridge::StatusCode::kNotFound);
}

}  // namespace
