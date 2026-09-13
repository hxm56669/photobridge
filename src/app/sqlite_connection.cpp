#include "photobridge/app/sqlite_connection.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace photobridge {
namespace {

Status SqliteError(
    sqlite3* database,
    std::string_view operation)
{
    const char* message = database == nullptr
        ? "unknown SQLite error"
        : sqlite3_errmsg(database);
    return Status(
        StatusCode::kIoError,
        std::string(operation) + ": " + message);
}

StatusOr<std::string> ReadTextPragma(
    sqlite3* database,
    const char* sql,
    std::string_view operation)
{
    sqlite3_stmt* statement = nullptr;
    const int prepare_result = sqlite3_prepare_v2(
        database,
        sql,
        -1,
        &statement,
        nullptr);
    if (prepare_result != SQLITE_OK) {
        return SqliteError(database, operation);
    }

    const int step_result = sqlite3_step(statement);
    if (step_result != SQLITE_ROW) {
        const Status status = SqliteError(database, operation);
        sqlite3_finalize(statement);
        return status;
    }

    const unsigned char* value = sqlite3_column_text(statement, 0);
    if (value == nullptr) {
        sqlite3_finalize(statement);
        return Status(
            StatusCode::kInternal,
            std::string(operation) + " returned NULL");
    }

    std::string result(reinterpret_cast<const char*>(value));
    const int final_step = sqlite3_step(statement);
    if (final_step != SQLITE_DONE) {
        const Status status = SqliteError(database, operation);
        sqlite3_finalize(statement);
        return status;
    }

    sqlite3_finalize(statement);
    return result;
}

StatusOr<int> ReadIntegerPragma(
    sqlite3* database,
    const char* sql,
    std::string_view operation)
{
    sqlite3_stmt* statement = nullptr;
    const int prepare_result = sqlite3_prepare_v2(
        database,
        sql,
        -1,
        &statement,
        nullptr);
    if (prepare_result != SQLITE_OK) {
        return SqliteError(database, operation);
    }

    const int step_result = sqlite3_step(statement);
    if (step_result != SQLITE_ROW) {
        const Status status = SqliteError(database, operation);
        sqlite3_finalize(statement);
        return status;
    }

    if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER) {
        sqlite3_finalize(statement);
        return Status(
            StatusCode::kInternal,
            std::string(operation) + " returned a non-integer value");
    }

    const int value = sqlite3_column_int(statement, 0);
    const int final_step = sqlite3_step(statement);
    if (final_step != SQLITE_DONE) {
        const Status status = SqliteError(database, operation);
        sqlite3_finalize(statement);
        return status;
    }

    sqlite3_finalize(statement);
    return value;
}

Status Configure(sqlite3* database)
{
    Status status = Status::Ok();

    status = Status(
        sqlite3_exec(
            database,
            "PRAGMA journal_mode=WAL;",
            nullptr,
            nullptr,
            nullptr) == SQLITE_OK
            ? StatusCode::kOk
            : StatusCode::kIoError,
        "configure SQLite journal mode");
    if (!status.ok()) {
        return SqliteError(database, status.message());
    }

    auto journal_mode = ReadTextPragma(
        database,
        "PRAGMA journal_mode;",
        "read SQLite journal mode");
    if (!journal_mode.ok()) {
        return journal_mode.status();
    }
    std::transform(
        journal_mode.value().begin(),
        journal_mode.value().end(),
        journal_mode.value().begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    if (journal_mode.value() != "wal") {
        return Status(
            StatusCode::kIoError,
            "SQLite journal mode is not WAL: " + journal_mode.value());
    }

    status = Status(
        sqlite3_exec(
            database,
            "PRAGMA synchronous=FULL;",
            nullptr,
            nullptr,
            nullptr) == SQLITE_OK
            ? StatusCode::kOk
            : StatusCode::kIoError,
        "configure SQLite synchronous mode");
    if (!status.ok()) {
        return SqliteError(database, status.message());
    }

    auto synchronous = ReadIntegerPragma(
        database,
        "PRAGMA synchronous;",
        "read SQLite synchronous mode");
    if (!synchronous.ok()) {
        return synchronous.status();
    }
    if (synchronous.value() != 2) {
        return Status(
            StatusCode::kIoError,
            "SQLite synchronous mode is not FULL");
    }

    status = Status(
        sqlite3_exec(
            database,
            "PRAGMA foreign_keys=ON;",
            nullptr,
            nullptr,
            nullptr) == SQLITE_OK
            ? StatusCode::kOk
            : StatusCode::kIoError,
        "configure SQLite foreign keys");
    if (!status.ok()) {
        return SqliteError(database, status.message());
    }

    auto foreign_keys = ReadIntegerPragma(
        database,
        "PRAGMA foreign_keys;",
        "read SQLite foreign keys");
    if (!foreign_keys.ok()) {
        return foreign_keys.status();
    }
    if (foreign_keys.value() != 1) {
        return Status(
            StatusCode::kIoError,
            "SQLite foreign keys are not enabled");
    }

    if (sqlite3_busy_timeout(database, 5000) != SQLITE_OK) {
        return SqliteError(database, "configure SQLite busy timeout");
    }

    return Status::Ok();
}

}  // namespace

SqliteConnection::SqliteConnection(sqlite3* handle) noexcept
    : handle_(handle) {}

SqliteConnection::~SqliteConnection()
{
    if (handle_ != nullptr) {
        sqlite3_close(handle_);
    }
}

SqliteConnection::SqliteConnection(SqliteConnection&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

SqliteConnection& SqliteConnection::operator=(SqliteConnection&& other) noexcept
{
    if (this == &other) {
        return *this;
    }

    if (handle_ != nullptr) {
        sqlite3_close(handle_);
    }

    handle_ = std::exchange(other.handle_, nullptr);
    return *this;
}

StatusOr<SqliteConnection> SqliteConnection::Open(
    const std::filesystem::path& path)
{
    if (path.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "SQLite database path must not be empty");
    }

    sqlite3* database = nullptr;
    const int result = sqlite3_open_v2(
        path.string().c_str(),
        &database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    if (result != SQLITE_OK) {
        const Status status = SqliteError(database, "open SQLite database");
        if (database != nullptr) {
            sqlite3_close(database);
        }
        return status;
    }

    const Status configuration = Configure(database);
    if (!configuration.ok()) {
        sqlite3_close(database);
        return configuration;
    }

    return SqliteConnection(database);
}

Status SqliteConnection::Execute(std::string_view sql) const
{
    if (handle_ == nullptr) {
        return Status(
            StatusCode::kInternal,
            "cannot execute SQL on a closed SQLite connection");
    }

    char* error_message = nullptr;
    const int result = sqlite3_exec(
        handle_,
        std::string(sql).c_str(),
        nullptr,
        nullptr,
        &error_message);
    if (result != SQLITE_OK) {
        const std::string message = error_message == nullptr
            ? sqlite3_errmsg(handle_)
            : error_message;
        sqlite3_free(error_message);
        return Status(
            StatusCode::kIoError,
            std::string("execute SQLite statement: ") + message);
    }

    return Status::Ok();
}

sqlite3* SqliteConnection::native_handle() const noexcept
{
    return handle_;
}

}  // namespace photobridge
