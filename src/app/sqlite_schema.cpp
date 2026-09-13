#include "photobridge/app/sqlite_schema.h"

#include <sqlite3.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

StatusOr<bool> HasSchemaVersionTable(sqlite3* database)
{
    sqlite3_stmt* statement = nullptr;
    const int prepare_result = sqlite3_prepare_v2(
        database,
        "SELECT 1 FROM sqlite_master "
        "WHERE type = 'table' AND name = 'schema_version' LIMIT 1;",
        -1,
        &statement,
        nullptr);
    if (prepare_result != SQLITE_OK) {
        return SqliteError(database, "inspect SQLite schema");
    }

    const int step_result = sqlite3_step(statement);
    const bool exists = step_result == SQLITE_ROW;
    if (step_result != SQLITE_ROW && step_result != SQLITE_DONE) {
        sqlite3_finalize(statement);
        return SqliteError(database, "inspect SQLite schema");
    }

    sqlite3_finalize(statement);
    return exists;
}

StatusOr<std::optional<std::int64_t>> ReadSchemaVersion(sqlite3* database)
{
    sqlite3_stmt* statement = nullptr;
    const int prepare_result = sqlite3_prepare_v2(
        database,
        "SELECT version FROM schema_version;",
        -1,
        &statement,
        nullptr);
    if (prepare_result != SQLITE_OK) {
        return SqliteError(database, "read SQLite schema version");
    }

    const int first_step = sqlite3_step(statement);
    if (first_step == SQLITE_DONE) {
        sqlite3_finalize(statement);
        return std::optional<std::int64_t>();
    }
    if (first_step != SQLITE_ROW) {
        sqlite3_finalize(statement);
        return SqliteError(database, "read SQLite schema version");
    }

    if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER) {
        sqlite3_finalize(statement);
        return Status(
            StatusCode::kInternal,
            "SQLite schema version is not an integer");
    }

    const std::int64_t version = sqlite3_column_int64(statement, 0);
    const int second_step = sqlite3_step(statement);
    if (second_step == SQLITE_ROW) {
        sqlite3_finalize(statement);
        return Status(
            StatusCode::kInternal,
            "SQLite schema version table must contain exactly one row");
    }
    if (second_step != SQLITE_DONE) {
        sqlite3_finalize(statement);
        return SqliteError(database, "read SQLite schema version");
    }

    sqlite3_finalize(statement);
    return std::optional<std::int64_t>(version);
}

Status RollbackAndReturn(SqliteConnection& connection, Status status)
{
    connection.Execute("ROLLBACK;");
    return status;
}

Status ApplySchemaV2(SqliteConnection& connection)
{
    Status status = connection.Execute(
        "CREATE TABLE source_manifest ("
        "manifest_id TEXT PRIMARY KEY,"
        "source_id TEXT NOT NULL,"
        "source_type TEXT NOT NULL,"
        "source_root BLOB NOT NULL,"
        "state INTEGER NOT NULL,"
        "manifest_digest BLOB,"
        "created_at_ns INTEGER NOT NULL"
        ");");
    if (!status.ok()) {
        return status;
    }

    status = connection.Execute(
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
        "UNIQUE (manifest_id, relative_path),"
        "FOREIGN KEY (manifest_id) "
        "REFERENCES source_manifest(manifest_id)"
        ");");
    if (!status.ok()) {
        return status;
    }

    return connection.Execute(
        "UPDATE schema_version SET version = 2;");
}

Status ApplySchemaV3(SqliteConnection& connection)
{
    Status status = connection.Execute(
        "CREATE TABLE migration ("
        "migration_id TEXT PRIMARY KEY,"
        "source_manifest_id TEXT NOT NULL,"
        "target_root BLOB NOT NULL,"
        "state INTEGER NOT NULL,"
        "current_epoch INTEGER NOT NULL DEFAULT 0,"
        "created_at_ns INTEGER NOT NULL"
        ");");
    if (!status.ok()) {
        return status;
    }

    status = connection.Execute(
        "CREATE TABLE migration_plan ("
        "plan_id TEXT PRIMARY KEY,"
        "migration_id TEXT NOT NULL,"
        "plan_path BLOB NOT NULL,"
        "artifact_digest BLOB NOT NULL,"
        "semantic_digest BLOB NOT NULL,"
        "semantic_profile_version INTEGER NOT NULL,"
        "format_version INTEGER NOT NULL,"
        "state INTEGER NOT NULL,"
        "created_at_ns INTEGER NOT NULL,"
        "FOREIGN KEY (migration_id) REFERENCES migration(migration_id)"
        ");");
    if (!status.ok()) {
        return status;
    }

    status = connection.Execute(
        "CREATE TABLE plan_task ("
        "plan_id TEXT NOT NULL,"
        "task_id TEXT NOT NULL,"
        "task_key TEXT NOT NULL,"
        "type INTEGER NOT NULL,"
        "state INTEGER NOT NULL,"
        "owner_epoch INTEGER,"
        "active_attempt_id TEXT,"
        "attempt_count INTEGER NOT NULL DEFAULT 0,"
        "target_path BLOB NOT NULL,"
        "expected_size INTEGER,"
        "expected_digest BLOB,"
        "last_error_code INTEGER,"
        "last_error_message TEXT,"
        "PRIMARY KEY (plan_id, task_id),"
        "UNIQUE (plan_id, task_key),"
        "FOREIGN KEY (plan_id) REFERENCES migration_plan(plan_id)"
        ");");
    if (!status.ok()) {
        return status;
    }

    status = connection.Execute(
        "CREATE TABLE task_dependency ("
        "plan_id TEXT NOT NULL,"
        "task_id TEXT NOT NULL,"
        "depends_on_task_id TEXT NOT NULL,"
        "PRIMARY KEY (plan_id, task_id, depends_on_task_id),"
        "FOREIGN KEY (plan_id, task_id) "
        "REFERENCES plan_task(plan_id, task_id),"
        "FOREIGN KEY (plan_id, depends_on_task_id) "
        "REFERENCES plan_task(plan_id, task_id)"
        ");");
    if (!status.ok()) {
        return status;
    }

    status = connection.Execute(
        "CREATE TABLE task_attempt ("
        "attempt_id TEXT PRIMARY KEY,"
        "plan_id TEXT NOT NULL,"
        "task_id TEXT NOT NULL,"
        "owner_epoch INTEGER NOT NULL,"
        "file_state INTEGER,"
        "started_at_ns INTEGER NOT NULL,"
        "finished_at_ns INTEGER,"
        "result INTEGER,"
        "error_code INTEGER,"
        "error_message TEXT,"
        "FOREIGN KEY (plan_id, task_id) "
        "REFERENCES plan_task(plan_id, task_id)"
        ");");
    if (!status.ok()) {
        return status;
    }

    return connection.Execute(
        "UPDATE schema_version SET version = 3;");
}

Status ApplySchemaV4(SqliteConnection& connection)
{
    Status status = connection.Execute(
        "CREATE TABLE task_event ("
        "event_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "plan_id TEXT NOT NULL,"
        "task_id TEXT NOT NULL,"
        "event_type TEXT NOT NULL,"
        "owner_epoch INTEGER,"
        "attempt_id TEXT,"
        "detail TEXT NOT NULL DEFAULT '',"
        "created_at_ns INTEGER NOT NULL DEFAULT 0,"
        "FOREIGN KEY (plan_id, task_id) "
        "REFERENCES plan_task(plan_id, task_id)"
        ");");
    if (!status.ok()) return status;
    status = connection.Execute(
        "CREATE INDEX task_event_plan_task_idx "
        "ON task_event(plan_id, task_id, event_id);");
    if (!status.ok()) return status;
    return connection.Execute("UPDATE schema_version SET version = 4;");
}

Status ApplySchemaV5(SqliteConnection& connection)
{
    Status status = connection.Execute(
        "CREATE TABLE verified_receipt ("
        "plan_id TEXT NOT NULL,"
        "task_id TEXT NOT NULL,"
        "attempt_id TEXT NOT NULL,"
        "owner_epoch INTEGER NOT NULL,"
        "temp_path BLOB NOT NULL,"
        "final_path BLOB NOT NULL,"
        "content_size INTEGER NOT NULL CHECK(content_size >= 0),"
        "source_digest BLOB NOT NULL CHECK(length(source_digest) = 32),"
        "target_digest BLOB NOT NULL CHECK(length(target_digest) = 32),"
        "source_identity BLOB NOT NULL,"
        "PRIMARY KEY(plan_id, task_id, attempt_id),"
        "FOREIGN KEY(plan_id, task_id) REFERENCES plan_task(plan_id, task_id),"
        "FOREIGN KEY(attempt_id) REFERENCES task_attempt(attempt_id)"
        ");");
    if (!status.ok()) return status;
    status = connection.Execute(
        "CREATE INDEX verified_receipt_task_idx "
        "ON verified_receipt(plan_id, task_id, attempt_id);");
    if (!status.ok()) return status;
    return connection.Execute("UPDATE schema_version SET version = 5;");
}

}  // namespace

Status EnsureSchema(SqliteConnection& connection)
{
    Status status = connection.Execute("BEGIN IMMEDIATE;");
    if (!status.ok()) {
        return status;
    }

    auto table_exists = HasSchemaVersionTable(connection.native_handle());
    if (!table_exists.ok()) {
        return RollbackAndReturn(connection, table_exists.status());
    }

    std::int64_t version_value = 0;
    if (!table_exists.value()) {
        status = connection.Execute(
            "CREATE TABLE schema_version ("
            "version INTEGER NOT NULL"
            ");");
        if (!status.ok()) {
            return RollbackAndReturn(connection, status);
        }

        status = connection.Execute(
            "INSERT INTO schema_version(version) VALUES (1);");
        if (!status.ok()) {
            return RollbackAndReturn(connection, status);
        }

        version_value = 1;
    } else {
        auto version = ReadSchemaVersion(connection.native_handle());
        if (!version.ok()) {
            return RollbackAndReturn(connection, version.status());
        }
        if (!version.value().has_value()) {
            return RollbackAndReturn(
                connection,
                Status(
                    StatusCode::kInternal,
                    "SQLite schema version table is empty"));
        }
        version_value = version.value().value();
        if (version_value > kCurrentSchemaVersion) {
            return RollbackAndReturn(
                connection,
                Status(
                    StatusCode::kInternal,
                    "SQLite schema version is newer than this binary"));
        }
    }

    while (version_value < kCurrentSchemaVersion) {
        if (version_value == 1) {
            status = ApplySchemaV2(connection);
            if (!status.ok()) {
                return RollbackAndReturn(connection, status);
            }
            version_value = 2;
            continue;
        }

        if (version_value == 2) {
            status = ApplySchemaV3(connection);
            if (!status.ok()) {
                return RollbackAndReturn(connection, status);
            }
            version_value = 3;
            continue;
        }

        if (version_value == 3) {
            status = ApplySchemaV4(connection);
            if (!status.ok()) {
                return RollbackAndReturn(connection, status);
            }
            version_value = 4;
            continue;
        }

        if (version_value == 4) {
            status = ApplySchemaV5(connection);
            if (!status.ok()) {
                return RollbackAndReturn(connection, status);
            }
            version_value = 5;
            continue;
        }

        return RollbackAndReturn(
            connection,
            Status(
                StatusCode::kInternal,
                "SQLite schema version is unsupported by this binary"));
    }

    status = connection.Execute("COMMIT;");
    if (!status.ok()) {
        connection.Execute("ROLLBACK;");
    }
    return status;
}

}  // namespace photobridge
