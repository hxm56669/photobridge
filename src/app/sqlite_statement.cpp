#include "photobridge/app/sqlite_statement.h"

namespace photobridge {

SqliteStatement::SqliteStatement(sqlite3* database, const char* sql) noexcept
{
    Prepare(database, sql);
}

SqliteStatement::~SqliteStatement()
{
    if (statement_ != nullptr) {
        sqlite3_finalize(statement_);
    }
}

int SqliteStatement::Prepare(sqlite3* database, const char* sql) noexcept
{
    if (statement_ == nullptr) {
        result_ = sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr);
    }
    return result_;
}

int SqliteStatement::Reset() noexcept
{
    if (statement_ == nullptr) {
        return SQLITE_MISUSE;
    }
    // sqlite3_reset reports the previous sqlite3_step error. That error was
    // handled by the caller; clear the bindings so the statement can be reused.
    sqlite3_reset(statement_);
    return sqlite3_clear_bindings(statement_);
}

}  // namespace photobridge
