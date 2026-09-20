#include "photobridge/app/sqlite_schema.h"

#include <sqlite3.h>

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace {

class SqliteSchemaTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        database_path_ = std::filesystem::temp_directory_path()
            / ("photobridge_sqlite_schema_"
                + std::to_string(++sequence_)
                + ".db");
        std::error_code error;
        std::filesystem::remove(database_path_, error);
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove(database_path_, error);
    }

    std::filesystem::path database_path_;
    static inline int sequence_ = 0;
};

TEST_F(SqliteSchemaTest, InitializesNewDatabaseAtCurrentVersion)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();

    const photobridge::Status status =
        photobridge::EnsureSchema(connection.value());
    ASSERT_TRUE(status.ok()) << status.message();

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            connection.value().native_handle(),
            "SELECT version FROM schema_version;",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0),
        photobridge::kCurrentSchemaVersion);
    EXPECT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);
}

TEST_F(SqliteSchemaTest, MigratesVersionOneToCurrentVersion)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(connection.value().Execute(
        "CREATE TABLE schema_version (version INTEGER NOT NULL);"
        "INSERT INTO schema_version(version) VALUES (1);").ok());

    const photobridge::Status status =
        photobridge::EnsureSchema(connection.value());
    ASSERT_TRUE(status.ok()) << status.message();

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            connection.value().native_handle(),
            "SELECT version FROM schema_version;",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(
        sqlite3_column_int(statement, 0),
        photobridge::kCurrentSchemaVersion);
    sqlite3_finalize(statement);

    ASSERT_EQ(
        sqlite3_prepare_v2(
            connection.value().native_handle(),
            "SELECT COUNT(*) FROM sqlite_master "
            "WHERE type = 'table' AND name IN "
            "('source_manifest', 'physical_asset');",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0), 2);
    sqlite3_finalize(statement);
}

TEST_F(SqliteSchemaTest, CreatesTaskRuntimeTablesAtCurrentVersion)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            connection.value().native_handle(),
            "SELECT COUNT(*) FROM sqlite_master "
            "WHERE type = 'table' AND name IN "
            "('migration', 'migration_plan', 'plan_task', "
            "'task_dependency', 'task_attempt', 'task_event', "
            "'verified_receipt');",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0), 7);
    sqlite3_finalize(statement);
}

TEST_F(SqliteSchemaTest, MigratesVersionTwoToTaskRuntimeSchema)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(connection.value().Execute(
        "CREATE TABLE schema_version (version INTEGER NOT NULL);"
        "INSERT INTO schema_version(version) VALUES (2);"
        "CREATE TABLE source_manifest ("
        "manifest_id TEXT PRIMARY KEY,"
        "source_id TEXT NOT NULL,"
        "source_type TEXT NOT NULL,"
        "source_root BLOB NOT NULL,"
        "state INTEGER NOT NULL,"
        "manifest_digest BLOB,"
        "created_at_ns INTEGER NOT NULL"
        ");"
        "CREATE TABLE physical_asset ("
        "manifest_id TEXT NOT NULL,"
        "asset_id TEXT NOT NULL,"
        "relative_path BLOB NOT NULL,"
        "display_path TEXT NOT NULL,"
        "device INTEGER NOT NULL,"
        "inode INTEGER NOT NULL,"
        "size INTEGER NOT NULL,"
        "mtime_ns INTEGER NOT NULL,"
        "ctime_ns INTEGER NOT NULL,"
        "kind INTEGER NOT NULL,"
        "PRIMARY KEY (manifest_id, asset_id),"
        "UNIQUE (manifest_id, relative_path)"
        ");").ok());

    const photobridge::Status status =
        photobridge::EnsureSchema(connection.value());
    ASSERT_TRUE(status.ok()) << status.message();

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            connection.value().native_handle(),
            "SELECT version FROM schema_version;",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(
        sqlite3_column_int(statement, 0),
        photobridge::kCurrentSchemaVersion);
    sqlite3_finalize(statement);
}

TEST_F(SqliteSchemaTest, RejectsNewerSchemaVersion)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(connection.value().Execute(
        "CREATE TABLE schema_version (version INTEGER NOT NULL);"
        "INSERT INTO schema_version(version) VALUES (999);").ok());

    const photobridge::Status status =
        photobridge::EnsureSchema(connection.value());
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), photobridge::StatusCode::kInternal);
}

TEST_F(SqliteSchemaTest, RejectsOlderSchemaVersionWithoutGuessingMigration)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(connection.value().Execute(
        "CREATE TABLE schema_version (version INTEGER NOT NULL);"
        "INSERT INTO schema_version(version) VALUES (0);").ok());

    const photobridge::Status status =
        photobridge::EnsureSchema(connection.value());

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), photobridge::StatusCode::kInternal);
}

TEST_F(SqliteSchemaTest, RejectsEmptySchemaVersionTable)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(connection.value().Execute(
        "CREATE TABLE schema_version (version INTEGER NOT NULL);").ok());

    const photobridge::Status status =
        photobridge::EnsureSchema(connection.value());

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), photobridge::StatusCode::kInternal);
}

TEST_F(SqliteSchemaTest, RejectsMultipleSchemaVersionRows)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(connection.value().Execute(
        "CREATE TABLE schema_version (version INTEGER NOT NULL);"
        "INSERT INTO schema_version(version) VALUES (1), (1);").ok());

    const photobridge::Status status =
        photobridge::EnsureSchema(connection.value());

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), photobridge::StatusCode::kInternal);
}

}  // namespace
