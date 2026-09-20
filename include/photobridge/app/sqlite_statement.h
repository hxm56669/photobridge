#pragma once

#include <sqlite3.h>

namespace photobridge {

// Owns a prepared statement. The connection must outlive this object.
class SqliteStatement final {
public:
    SqliteStatement() = default;
    SqliteStatement(sqlite3* database, const char* sql) noexcept;
    ~SqliteStatement();

    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;

    // A cached statement is prepared only on its first use.
    int Prepare(sqlite3* database, const char* sql) noexcept;
    // Reset discards the previous step result and clears all bound values.
    int Reset() noexcept;

    int result() const noexcept { return result_; }
    sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3_stmt* statement_ = nullptr;
    int result_ = SQLITE_OK;
};

}  // namespace photobridge
