#include "photobridge/app/sqlite_connection.h"

#include <sqlite3.h>

#include <filesystem>
#include <cstdint>
#include <string>
#include <utility>

#include <gtest/gtest.h>

namespace {

class SqliteConnectionTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        database_path_ = std::filesystem::temp_directory_path()
            / ("photobridge_sqlite_connection_"
                + std::to_string(static_cast<long long>(
                    reinterpret_cast<std::uintptr_t>(this)))
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
};

int QueryIntegerPragma(sqlite3* database, const char* sql)
{
    sqlite3_stmt* statement = nullptr;
    EXPECT_EQ(
        sqlite3_prepare_v2(database, sql, -1, &statement, nullptr),
        SQLITE_OK);
    if (statement == nullptr) {
        return -1;
    }

    const int step_result = sqlite3_step(statement);
    EXPECT_EQ(step_result, SQLITE_ROW);
    const int value = step_result == SQLITE_ROW
        ? sqlite3_column_int(statement, 0)
        : -1;
    sqlite3_finalize(statement);
    return value;
}

std::string QueryTextPragma(sqlite3* database, const char* sql)
{
    sqlite3_stmt* statement = nullptr;
    EXPECT_EQ(
        sqlite3_prepare_v2(database, sql, -1, &statement, nullptr),
        SQLITE_OK);
    if (statement == nullptr) {
        return {};
    }

    const int step_result = sqlite3_step(statement);
    EXPECT_EQ(step_result, SQLITE_ROW);
    const unsigned char* value = step_result == SQLITE_ROW
        ? sqlite3_column_text(statement, 0)
        : nullptr;
    const std::string result = value == nullptr
        ? std::string{}
        : std::string(reinterpret_cast<const char*>(value));
    sqlite3_finalize(statement);
    return result;
}

TEST_F(SqliteConnectionTest, OpensAndAppliesDurabilityPragmas)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);

    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_NE(connection.value().native_handle(), nullptr);

    EXPECT_EQ(
        QueryIntegerPragma(connection.value().native_handle(),
            "PRAGMA synchronous;"),
        2);
    EXPECT_EQ(
        QueryIntegerPragma(connection.value().native_handle(),
            "PRAGMA foreign_keys;"),
        1);
    EXPECT_EQ(
        QueryTextPragma(connection.value().native_handle(),
            "PRAGMA journal_mode;"),
        "wal");

    const auto create_status = connection.value().Execute(
        "CREATE TABLE connection_test (value INTEGER NOT NULL);");
    EXPECT_TRUE(create_status.ok()) << create_status.message();
}

TEST_F(SqliteConnectionTest, RejectsEmptyPath)
{
    const auto connection =
        photobridge::SqliteConnection::Open(std::filesystem::path{});

    ASSERT_FALSE(connection.ok());
    EXPECT_EQ(
        connection.status().code(),
        photobridge::StatusCode::kInvalidArgument);
}

TEST_F(SqliteConnectionTest, IsMoveOnly)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();

    photobridge::SqliteConnection moved = std::move(connection.value());
    ASSERT_NE(moved.native_handle(), nullptr);
    EXPECT_EQ(connection.value().native_handle(), nullptr);
}

}  // namespace
