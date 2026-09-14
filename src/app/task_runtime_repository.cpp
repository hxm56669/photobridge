#include "photobridge/app/task_runtime_repository.h"

#include <sqlite3.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "photobridge/common/time.h"

namespace photobridge {
namespace {

Status Invalid(const char* message)
{
    return Status(StatusCode::kInvalidArgument, message);
}

Status SqliteError(sqlite3* database, std::string_view operation)
{
    return Status(
        StatusCode::kIoError,
        std::string(operation) + ": " + sqlite3_errmsg(database));
}

Status Rollback(SqliteConnection& connection, Status status)
{
    connection.Execute("ROLLBACK;");
    return status;
}

Status CheckIds(std::string_view plan_id, std::string_view task_id)
{
    if (plan_id.empty() || task_id.empty()) {
        return Invalid("plan and task ids must not be empty");
    }
    if (plan_id.find('\0') != std::string_view::npos
        || task_id.find('\0') != std::string_view::npos) {
        return Invalid("plan and task ids must not contain NUL");
    }
    return Status::Ok();
}

Status CheckEpochAttempt(ExecutionEpoch epoch, std::string_view attempt_id)
{
    if (epoch.value == 0 || attempt_id.empty()
        || attempt_id.find('\0') != std::string_view::npos) {
        return Invalid("task claim requires a positive epoch and attempt id");
    }
    if (epoch.value > std::numeric_limits<sqlite3_int64>::max()) {
        return Invalid("execution epoch exceeds SQLite integer range");
    }
    return Status::Ok();
}

class Statement final {
public:
    Statement(sqlite3* database, const char* sql) noexcept
    {
        result_ = sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr);
    }

    ~Statement()
    {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    int result() const noexcept { return result_; }
    sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3_stmt* statement_ = nullptr;
    int result_ = SQLITE_ERROR;
};

Status BindText(sqlite3_stmt* statement, int index, std::string_view value)
{
    if (sqlite3_bind_text(
            statement,
            index,
            value.data(),
            static_cast<int>(value.size()),
            SQLITE_TRANSIENT)
        != SQLITE_OK) {
        return Status(StatusCode::kInternal, "SQLite failed to bind text");
    }
    return Status::Ok();
}

Status BindBlob(
    sqlite3_stmt* statement,
    int index,
    const void* data,
    std::size_t size)
{
    if (sqlite3_bind_blob(
            statement,
            index,
            data,
            static_cast<int>(size),
            SQLITE_TRANSIENT)
        != SQLITE_OK) {
        return Status(StatusCode::kInternal, "SQLite failed to bind blob");
    }
    return Status::Ok();
}

std::array<std::byte, 49> EncodeIdentity(const FileIdentity& identity)
{
    std::array<std::byte, 49> encoded{};
    std::memcpy(encoded.data() + 0, &identity.device, sizeof(identity.device));
    std::memcpy(encoded.data() + 8, &identity.inode, sizeof(identity.inode));
    std::memcpy(encoded.data() + 16, &identity.size, sizeof(identity.size));
    std::memcpy(encoded.data() + 24, &identity.mtime_ns, sizeof(identity.mtime_ns));
    std::memcpy(encoded.data() + 32, &identity.ctime_ns, sizeof(identity.ctime_ns));
    encoded[40] = identity.mount_id.has_value()
        ? std::byte{1}
        : std::byte{0};
    if (identity.mount_id.has_value()) {
        std::memcpy(
            encoded.data() + 41,
            &identity.mount_id.value(),
            sizeof(identity.mount_id.value()));
    }
    return encoded;
}

StatusOr<FileIdentity> DecodeIdentity(const void* data, int size)
{
    if (data == nullptr || size != 49) {
        return Status(
            StatusCode::kInternal,
            "verified receipt has an invalid source identity");
    }
    FileIdentity identity;
    const auto* bytes = static_cast<const std::byte*>(data);
    std::memcpy(&identity.device, bytes + 0, sizeof(identity.device));
    std::memcpy(&identity.inode, bytes + 8, sizeof(identity.inode));
    std::memcpy(&identity.size, bytes + 16, sizeof(identity.size));
    std::memcpy(&identity.mtime_ns, bytes + 24, sizeof(identity.mtime_ns));
    std::memcpy(&identity.ctime_ns, bytes + 32, sizeof(identity.ctime_ns));
    if (bytes[40] == std::byte{1}) {
        std::uint64_t mount_id = 0;
        std::memcpy(&mount_id, bytes + 41, sizeof(mount_id));
        identity.mount_id = mount_id;
    } else if (bytes[40] != std::byte{0}) {
        return Status(
            StatusCode::kInternal,
            "verified receipt has an invalid mount id marker");
    }
    return identity;
}

bool SameVerifiedReceipt(
    const VerifiedReceipt& left,
    const VerifiedReceipt& right)
{
    return left.task_id == right.task_id
        && left.attempt_id == right.attempt_id
        && left.owner_epoch == right.owner_epoch
        && left.temp_path == right.temp_path
        && left.final_path == right.final_path
        && left.content_size == right.content_size
        && left.source_digest == right.source_digest
        && left.target_digest == right.target_digest
        && left.source_identity == right.source_identity;
}

Status BindEpoch(
    sqlite3_stmt* statement,
    int index,
    ExecutionEpoch epoch)
{
    if (sqlite3_bind_int64(
            statement,
            index,
            static_cast<sqlite3_int64>(epoch.value))
        != SQLITE_OK) {
        return Status(StatusCode::kInternal, "SQLite failed to bind epoch");
    }
    return Status::Ok();
}

Status AppendTaskEvent(
    SqliteConnection& connection,
    std::string_view plan_id,
    std::string_view task_id,
    std::string_view event_type,
    ExecutionEpoch epoch,
    std::string_view attempt_id,
    std::string_view detail)
{
    Statement statement(
        connection.native_handle(),
        "INSERT INTO task_event("
        "plan_id, task_id, event_type, owner_epoch, attempt_id, detail, "
        "created_at_ns) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7);");
    if (statement.result() != SQLITE_OK) {
        return SqliteError(
            connection.native_handle(),
            "prepare task event insert");
    }
    Status status = BindText(statement.get(), 1, plan_id);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 2, task_id);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 3, event_type);
    if (!status.ok()) return status;
    status = BindEpoch(statement.get(), 4, epoch);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 5, attempt_id);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 6, detail);
    if (!status.ok()) return status;
    if (sqlite3_bind_int64(
            statement.get(), 7, CurrentTimeNanoseconds()) != SQLITE_OK) {
        return Status(
            StatusCode::kInternal,
            "SQLite failed to bind task event timestamp");
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return SqliteError(connection.native_handle(), "insert task event");
    }
    return Status::Ok();
}

Status UpdateAttemptState(
    SqliteConnection& connection,
    std::string_view plan_id,
    std::string_view task_id,
    std::string_view attempt_id,
    ExecutionEpoch owner_epoch,
    FileAttemptState state,
    const Status* error,
    bool finish)
{
    Statement statement(
        connection.native_handle(),
        "UPDATE task_attempt SET file_state = ?5, finished_at_ns = ?6, "
        "error_code = ?7, error_message = ?8 "
        "WHERE plan_id = ?1 AND task_id = ?2 AND attempt_id = ?3 "
        "AND owner_epoch = ?4;");
    if (statement.result() != SQLITE_OK) {
        return SqliteError(
            connection.native_handle(), "prepare task attempt update");
    }
    Status status = BindText(statement.get(), 1, plan_id);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 2, task_id);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 3, attempt_id);
    if (!status.ok()) return status;
    status = BindEpoch(statement.get(), 4, owner_epoch);
    if (!status.ok()) return status;
    if (sqlite3_bind_int(
            statement.get(), 5, static_cast<int>(state)) != SQLITE_OK) {
        return Status(StatusCode::kInternal, "SQLite failed to bind attempt state");
    }
    if (finish) {
        if (sqlite3_bind_int64(
                statement.get(), 6, CurrentTimeNanoseconds()) != SQLITE_OK) {
            return Status(
                StatusCode::kInternal,
                "SQLite failed to bind attempt finish timestamp");
        }
    } else if (sqlite3_bind_null(statement.get(), 6) != SQLITE_OK) {
        return Status(
            StatusCode::kInternal,
            "SQLite failed to clear attempt finish timestamp");
    }
    if (error == nullptr) {
        if (sqlite3_bind_null(statement.get(), 7) != SQLITE_OK
            || sqlite3_bind_null(statement.get(), 8) != SQLITE_OK) {
            return Status(
                StatusCode::kInternal,
                "SQLite failed to clear attempt error");
        }
    } else {
        if (sqlite3_bind_int(
                statement.get(), 7, static_cast<int>(error->code())) != SQLITE_OK) {
            return Status(
                StatusCode::kInternal,
                "SQLite failed to bind attempt error code");
        }
        status = BindText(statement.get(), 8, error->message());
        if (!status.ok()) return status;
    }
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return SqliteError(connection.native_handle(), "update task attempt");
    }
    if (sqlite3_changes(connection.native_handle()) != 1) {
        return Invalid("task attempt was not found for state update");
    }
    return Status::Ok();
}

StatusOr<TaskRuntime> ReadRuntimeWithStatement(
    sqlite3* database,
    std::string_view plan_id,
    std::string_view task_id)
{
    Statement statement(
        database,
        "SELECT task_id, state, owner_epoch, active_attempt_id, "
        "attempt_count, last_error_code, last_error_message "
        "FROM plan_task WHERE plan_id = ?1 AND task_id = ?2;");
    if (statement.result() != SQLITE_OK) {
        return SqliteError(database, "prepare task runtime query");
    }
    Status status = BindText(statement.get(), 1, plan_id);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 2, task_id);
    if (!status.ok()) return status;

    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return Status(StatusCode::kNotFound, "task runtime was not found");
    }
    if (step != SQLITE_ROW) {
        return SqliteError(database, "read task runtime");
    }

    const int state = sqlite3_column_int(statement.get(), 1);
    if (state < static_cast<int>(TaskState::kPlanned)
        || state > static_cast<int>(TaskState::kSkipped)) {
        return Status(StatusCode::kInternal, "unknown persisted task state");
    }
    const sqlite3_int64 count = sqlite3_column_int64(statement.get(), 4);
    if (count < 0
        || static_cast<std::uint64_t>(count)
            > std::numeric_limits<std::uint32_t>::max()) {
        return Status(StatusCode::kInternal, "invalid persisted attempt count");
    }

    TaskRuntime runtime;
    runtime.id = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
    runtime.state = static_cast<TaskState>(state);
    runtime.attempt_count = static_cast<std::uint32_t>(count);
    if (sqlite3_column_type(statement.get(), 2) != SQLITE_NULL) {
        const sqlite3_int64 epoch = sqlite3_column_int64(statement.get(), 2);
        if (epoch < 0) {
            return Status(StatusCode::kInternal, "invalid persisted owner epoch");
        }
        runtime.owner_epoch.value = static_cast<std::uint64_t>(epoch);
    }
    if (sqlite3_column_type(statement.get(), 3) != SQLITE_NULL) {
        runtime.attempt_id = reinterpret_cast<const char*>(
            sqlite3_column_text(statement.get(), 3));
    }
    if (sqlite3_column_type(statement.get(), 5) != SQLITE_NULL
        || sqlite3_column_type(statement.get(), 6) != SQLITE_NULL) {
        if (sqlite3_column_type(statement.get(), 5) != SQLITE_INTEGER
            || sqlite3_column_type(statement.get(), 6) != SQLITE_TEXT) {
            return Status(StatusCode::kInternal, "invalid persisted task error");
        }
        const int error_code = sqlite3_column_int(statement.get(), 5);
        if (error_code <= static_cast<int>(StatusCode::kOk)
            || error_code > static_cast<int>(StatusCode::kInternal)) {
            return Status(StatusCode::kInternal, "unknown persisted error code");
        }
        runtime.last_error = Status(
            static_cast<StatusCode>(error_code),
            reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 6)));
    }
    return runtime;
}

StatusOr<ExecutionEpoch> ReadCurrentEpochForPlan(
    sqlite3* database,
    std::string_view plan_id);

Status FinishTask(
    SqliteConnection& connection,
    std::string_view plan_id,
    std::string_view task_id,
    ExecutionEpoch epoch,
    std::string_view attempt_id,
    TaskState next_state,
    const Status* error)
{
    Status status = connection.Execute("BEGIN IMMEDIATE;");
    if (!status.ok()) return status;
    const auto current = ReadCurrentEpochForPlan(
        connection.native_handle(), plan_id);
    if (!current.ok()) return Rollback(connection, current.status());
    if (current.value() != epoch) {
        return Rollback(connection, Invalid(
            "stale executor epoch cannot finish a task"));
    }

    Statement statement(
        connection.native_handle(),
        "UPDATE plan_task SET state = ?5, owner_epoch = NULL, "
        "active_attempt_id = NULL, last_error_code = ?6, "
        "last_error_message = ?7 "
        "WHERE plan_id = ?1 AND task_id = ?2 AND owner_epoch = ?3 "
        "AND active_attempt_id = ?4 AND state = ?8;");
    if (statement.result() != SQLITE_OK) {
        return Rollback(connection, SqliteError(
            connection.native_handle(), "prepare task completion"));
    }
    status = BindText(statement.get(), 1, plan_id);
    if (!status.ok()) return Rollback(connection, status);
    status = BindText(statement.get(), 2, task_id);
    if (!status.ok()) return Rollback(connection, status);
    status = BindEpoch(statement.get(), 3, epoch);
    if (!status.ok()) return Rollback(connection, status);
    status = BindText(statement.get(), 4, attempt_id);
    if (!status.ok()) return Rollback(connection, status);
    if (sqlite3_bind_int(statement.get(), 5, static_cast<int>(next_state))
            != SQLITE_OK
        || sqlite3_bind_int(
               statement.get(),
               8,
               static_cast<int>(TaskState::kRunning))
            != SQLITE_OK) {
        return Rollback(connection, Status(
            StatusCode::kInternal,
            "SQLite failed to bind completion state"));
    }
    if (error == nullptr) {
        if (sqlite3_bind_null(statement.get(), 6) != SQLITE_OK
            || sqlite3_bind_null(statement.get(), 7) != SQLITE_OK) {
            return Rollback(connection, Status(
                StatusCode::kInternal,
                "SQLite failed to clear task error"));
        }
    } else {
        if (sqlite3_bind_int(statement.get(), 6, static_cast<int>(error->code()))
                != SQLITE_OK) {
            return Rollback(connection, Status(
                StatusCode::kInternal,
                "SQLite failed to bind task error code"));
        }
        status = BindText(statement.get(), 7, error->message());
        if (!status.ok()) return Rollback(connection, status);
    }
    const int step = sqlite3_step(statement.get());
    if (step != SQLITE_DONE) {
        return Rollback(connection, SqliteError(
            connection.native_handle(), "complete task"));
    }
    if (sqlite3_changes(connection.native_handle()) != 1) {
        return Rollback(connection, Invalid(
            "task completion claim is stale or has an unexpected state"));
    }
    if (next_state == TaskState::kRetryable) {
        status = UpdateAttemptState(
            connection,
            plan_id,
            task_id,
            attempt_id,
            epoch,
            FileAttemptState::kRetryable,
            error,
            true);
        if (!status.ok()) return Rollback(connection, status);
    }
    status = AppendTaskEvent(
        connection,
        plan_id,
        task_id,
        next_state == TaskState::kSucceeded ? "SUCCEEDED" : "RETRYABLE",
        epoch,
        attempt_id,
        error == nullptr ? "completed" : error->message());
    if (!status.ok()) return Rollback(connection, status);
    status = connection.Execute("COMMIT;");
    if (!status.ok()) connection.Execute("ROLLBACK;");
    return status;
}

Status RecoverTask(
    SqliteConnection& connection,
    std::string_view plan_id,
    std::string_view task_id,
    ExecutionEpoch recovery_epoch,
    ExecutionEpoch expected_old_epoch,
    std::string_view expected_old_attempt_id,
    TaskState next_state,
    FileAttemptState attempt_state,
    std::string_view event_type,
    std::string_view reason,
    const Status* error)
{
    Status status = connection.Execute("BEGIN IMMEDIATE;");
    if (!status.ok()) return status;
    const auto current = ReadCurrentEpochForPlan(
        connection.native_handle(), plan_id);
    if (!current.ok()) return Rollback(connection, current.status());
    if (current.value() != recovery_epoch) {
        return Rollback(connection, Invalid(
            "stale recovery epoch cannot transition a task"));
    }

    Statement update(
        connection.native_handle(),
        "UPDATE plan_task SET state = ?6, owner_epoch = NULL, "
        "active_attempt_id = NULL, last_error_code = ?7, "
        "last_error_message = ?8 "
        "WHERE plan_id = ?1 AND task_id = ?2 AND owner_epoch = ?3 "
        "AND active_attempt_id = ?4 AND state = ?5;");
    if (update.result() != SQLITE_OK) {
        return Rollback(connection, SqliteError(
            connection.native_handle(), "prepare recovery transition"));
    }
    status = BindText(update.get(), 1, plan_id);
    if (!status.ok()) return Rollback(connection, status);
    status = BindText(update.get(), 2, task_id);
    if (!status.ok()) return Rollback(connection, status);
    status = BindEpoch(update.get(), 3, expected_old_epoch);
    if (!status.ok()) return Rollback(connection, status);
    status = BindText(update.get(), 4, expected_old_attempt_id);
    if (!status.ok()) return Rollback(connection, status);
    if (sqlite3_bind_int(
            update.get(), 5, static_cast<int>(TaskState::kRunning)) != SQLITE_OK
        || sqlite3_bind_int(
               update.get(), 6, static_cast<int>(next_state)) != SQLITE_OK) {
        return Rollback(connection, Status(
            StatusCode::kInternal,
            "SQLite failed to bind recovery transition state"));
    }
    if (error == nullptr) {
        if (sqlite3_bind_null(update.get(), 7) != SQLITE_OK
            || sqlite3_bind_null(update.get(), 8) != SQLITE_OK) {
            return Rollback(connection, Status(
                StatusCode::kInternal,
                "SQLite failed to clear recovery task error"));
        }
    } else {
        if (sqlite3_bind_int(
                update.get(), 7, static_cast<int>(error->code())) != SQLITE_OK) {
            return Rollback(connection, Status(
                StatusCode::kInternal,
                "SQLite failed to bind recovery task error code"));
        }
        status = BindText(update.get(), 8, error->message());
        if (!status.ok()) return Rollback(connection, status);
    }
    if (sqlite3_step(update.get()) != SQLITE_DONE) {
        return Rollback(connection, SqliteError(
            connection.native_handle(), "apply recovery transition"));
    }
    if (sqlite3_changes(connection.native_handle()) != 1) {
        return Rollback(connection, Invalid(
            "recovery ownership is stale or task is not running"));
    }

    status = UpdateAttemptState(
        connection,
        plan_id,
        task_id,
        expected_old_attempt_id,
        expected_old_epoch,
        attempt_state,
        error,
        true);
    if (!status.ok()) return Rollback(connection, status);

    const std::string detail =
        "old_epoch=" + std::to_string(expected_old_epoch.value)
        + " old_attempt=" + std::string(expected_old_attempt_id)
        + " reason=" + std::string(reason);
    status = AppendTaskEvent(
        connection,
        plan_id,
        task_id,
        event_type,
        recovery_epoch,
        expected_old_attempt_id,
        detail);
    if (!status.ok()) return Rollback(connection, status);
    status = connection.Execute("COMMIT;");
    if (!status.ok()) connection.Execute("ROLLBACK;");
    return status;
}

StatusOr<ExecutionEpoch> ReadCurrentEpochForPlan(
    sqlite3* database,
    std::string_view plan_id)
{
    Statement statement(
        database,
        "SELECT migration.current_epoch FROM migration_plan "
        "JOIN migration ON migration.migration_id = migration_plan.migration_id "
        "WHERE migration_plan.plan_id = ?1;");
    if (statement.result() != SQLITE_OK) {
        return SqliteError(database, "prepare current epoch query");
    }
    Status status = BindText(statement.get(), 1, plan_id);
    if (!status.ok()) return status;
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return Status(StatusCode::kNotFound, "migration plan was not found");
    }
    if (step != SQLITE_ROW) {
        return SqliteError(database, "read current epoch");
    }
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER) {
        return Status(
            StatusCode::kInternal,
            "persisted current epoch is not an integer");
    }
    const sqlite3_int64 epoch = sqlite3_column_int64(statement.get(), 0);
    if (epoch < 0) {
        return Status(
            StatusCode::kInternal,
            "persisted current epoch is negative");
    }
    return ExecutionEpoch{static_cast<std::uint64_t>(epoch)};
}

Status CheckTaskOwnership(
    sqlite3* database,
    std::string_view plan_id,
    std::string_view task_id,
    ExecutionEpoch epoch,
    std::string_view attempt_id)
{
    Statement statement(
        database,
        "SELECT state, owner_epoch, active_attempt_id FROM plan_task "
        "WHERE plan_id = ?1 AND task_id = ?2;");
    if (statement.result() != SQLITE_OK) {
        return SqliteError(database, "prepare task ownership query");
    }
    Status status = BindText(statement.get(), 1, plan_id);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 2, task_id);
    if (!status.ok()) return status;
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return Status(StatusCode::kNotFound, "task was not found");
    }
    if (step != SQLITE_ROW) {
        return SqliteError(database, "read task ownership");
    }
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER
        || sqlite3_column_type(statement.get(), 1) != SQLITE_INTEGER
        || sqlite3_column_type(statement.get(), 2) != SQLITE_TEXT) {
        return Status(StatusCode::kInternal, "task ownership has invalid columns");
    }
    if (sqlite3_column_int(statement.get(), 0)
            != static_cast<int>(TaskState::kRunning)
        || sqlite3_column_int64(statement.get(), 1)
            != static_cast<sqlite3_int64>(epoch.value)
        || std::string_view(reinterpret_cast<const char*>(
               sqlite3_column_text(statement.get(), 2))) != attempt_id) {
        return Invalid("task ownership is stale or does not match receipt");
    }
    return Status::Ok();
}

}  // namespace

TaskRuntimeRepository::TaskRuntimeRepository(
    SqliteConnection& connection) noexcept
    : connection_(&connection)
{
}

StatusOr<ExecutionEpoch> TaskRuntimeRepository::AcquireNextExecutionEpoch(
    const std::string& plan_id)
{
    if (plan_id.empty() || plan_id.find('\0') != std::string::npos) {
        return Invalid("plan id must be non-empty and NUL-free");
    }
    Status status = connection_->Execute("BEGIN IMMEDIATE;");
    if (!status.ok()) return status;
    const auto current = ReadCurrentEpochForPlan(
        connection_->native_handle(), plan_id);
    if (!current.ok()) return Rollback(*connection_, current.status());
    if (current.value().value
        >= static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return Rollback(*connection_, Invalid("execution epoch overflow"));
    }
    const ExecutionEpoch next{current.value().value + 1};
    Statement statement(
        connection_->native_handle(),
        "UPDATE migration SET current_epoch = ?2 WHERE migration_id = "
        "(SELECT migration_id FROM migration_plan WHERE plan_id = ?1);");
    if (statement.result() != SQLITE_OK) {
        return Rollback(*connection_, SqliteError(
            connection_->native_handle(), "prepare current epoch update"));
    }
    status = BindText(statement.get(), 1, plan_id);
    if (!status.ok()) return Rollback(*connection_, status);
    status = BindEpoch(statement.get(), 2, next);
    if (!status.ok()) return Rollback(*connection_, status);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return Rollback(*connection_, SqliteError(
            connection_->native_handle(), "advance current epoch"));
    }
    if (sqlite3_changes(connection_->native_handle()) != 1) {
        return Rollback(*connection_, Invalid(
            "migration plan epoch update changed no rows"));
    }
    status = connection_->Execute("COMMIT;");
    if (!status.ok()) connection_->Execute("ROLLBACK;");
    return status.ok() ? StatusOr<ExecutionEpoch>(next) : StatusOr<ExecutionEpoch>(status);
}

StatusOr<ExecutionEpoch> TaskRuntimeRepository::ReadCurrentEpoch(
    const std::string& plan_id) const
{
    if (plan_id.empty() || plan_id.find('\0') != std::string::npos) {
        return Invalid("plan id must be non-empty and NUL-free");
    }
    return ReadCurrentEpochForPlan(connection_->native_handle(), plan_id);
}

Status TaskRuntimeRepository::AddTask(
    const std::string& plan_id,
    const TaskSpec& task)
{
    Status status = CheckIds(plan_id, task.id);
    if (!status.ok()) return status;
    status = ValidateTaskSpec(task);
    if (!status.ok()) return status;
    if (task.estimated_bytes
        > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return Invalid("estimated task bytes exceed SQLite integer range");
    }

    Statement statement(
        connection_->native_handle(),
        "INSERT INTO plan_task("
        "plan_id, task_id, task_key, type, state, attempt_count, "
        "target_path, expected_size, expected_digest) "
        "VALUES(?1, ?2, ?3, ?4, ?5, 0, ?6, ?7, ?8);");
    if (statement.result() != SQLITE_OK) {
        return SqliteError(connection_->native_handle(), "prepare task insert");
    }
    status = BindText(statement.get(), 1, plan_id);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 2, task.id);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 3, task.task_key);
    if (!status.ok()) return status;
    if (sqlite3_bind_int(statement.get(), 4, static_cast<int>(task.type))
            != SQLITE_OK
        || sqlite3_bind_int(
               statement.get(),
               5,
               static_cast<int>(TaskState::kPlanned))
            != SQLITE_OK
        || sqlite3_bind_int64(
               statement.get(),
               7,
               static_cast<sqlite3_int64>(task.estimated_bytes))
            != SQLITE_OK) {
        return Status(StatusCode::kInternal, "SQLite failed to bind task fields");
    }
    if (sqlite3_bind_blob(
            statement.get(),
            6,
            task.target_path.bytes().data(),
            static_cast<int>(task.target_path.bytes().size()),
            SQLITE_TRANSIENT)
        != SQLITE_OK) {
        return Status(StatusCode::kInternal, "SQLite failed to bind target path");
    }
    if (task.expected_digest.has_value()) {
        if (sqlite3_bind_blob(
                statement.get(),
                8,
                task.expected_digest->bytes.data(),
                static_cast<int>(task.expected_digest->bytes.size()),
                SQLITE_TRANSIENT)
            != SQLITE_OK) {
            return Status(StatusCode::kInternal, "SQLite failed to bind digest");
        }
    } else if (sqlite3_bind_null(statement.get(), 8) != SQLITE_OK) {
        return Status(StatusCode::kInternal, "SQLite failed to bind null digest");
    }
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_CONSTRAINT) {
        return Status(
            StatusCode::kAlreadyExists,
            "task id or task key already exists in this plan");
    }
    if (step != SQLITE_DONE) {
        return SqliteError(connection_->native_handle(), "insert task");
    }
    return Status::Ok();
}

Status TaskRuntimeRepository::AddDependency(
    const std::string& plan_id,
    const TaskId& task,
    const TaskId& depends_on)
{
    Status status = CheckIds(plan_id, task);
    if (!status.ok()) return status;
    status = CheckIds(plan_id, depends_on);
    if (!status.ok()) return status;
    if (task == depends_on) {
        return Invalid("task graph must not contain a self dependency");
    }

    Statement cycle_check(
        connection_->native_handle(),
        "WITH RECURSIVE ancestors(task_id) AS ("
        "SELECT ?3 "
        "UNION "
        "SELECT d.depends_on_task_id FROM task_dependency d "
        "JOIN ancestors a ON d.task_id = a.task_id "
        "WHERE d.plan_id = ?1) "
        "SELECT 1 FROM ancestors WHERE task_id = ?2 LIMIT 1;");
    if (cycle_check.result() != SQLITE_OK) {
        return SqliteError(
            connection_->native_handle(),
            "prepare dependency cycle check");
    }
    status = BindText(cycle_check.get(), 1, plan_id);
    if (!status.ok()) return status;
    status = BindText(cycle_check.get(), 2, task);
    if (!status.ok()) return status;
    status = BindText(cycle_check.get(), 3, depends_on);
    if (!status.ok()) return status;
    const int cycle_step = sqlite3_step(cycle_check.get());
    if (cycle_step == SQLITE_ROW) {
        return Invalid("task graph dependency would create a cycle");
    }
    if (cycle_step != SQLITE_DONE) {
        return SqliteError(
            connection_->native_handle(),
            "check dependency cycle");
    }

    Statement statement(
        connection_->native_handle(),
        "INSERT INTO task_dependency(plan_id, task_id, depends_on_task_id) "
        "SELECT ?1, ?2, ?3 "
        "WHERE EXISTS(SELECT 1 FROM plan_task WHERE plan_id = ?1 AND task_id = ?2) "
        "AND EXISTS(SELECT 1 FROM plan_task WHERE plan_id = ?1 AND task_id = ?3);");
    if (statement.result() != SQLITE_OK) {
        return SqliteError(
            connection_->native_handle(),
            "prepare dependency insert");
    }
    status = BindText(statement.get(), 1, plan_id);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 2, task);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 3, depends_on);
    if (!status.ok()) return status;
    const int step = sqlite3_step(statement.get());
    if (step != SQLITE_DONE) {
        return SqliteError(connection_->native_handle(), "insert dependency");
    }
    if (sqlite3_changes(connection_->native_handle()) == 0) {
        return Status(
            StatusCode::kNotFound,
            "dependency references an unknown task");
    }
    return Status::Ok();
}

Status TaskRuntimeRepository::SetReady(
    const std::string& plan_id,
    const TaskId& task_id)
{
    Status status = CheckIds(plan_id, task_id);
    if (!status.ok()) return status;
    Statement statement(
        connection_->native_handle(),
        "UPDATE plan_task SET state = ?3, owner_epoch = NULL, "
        "active_attempt_id = NULL, last_error_code = NULL, "
        "last_error_message = NULL "
        "WHERE plan_id = ?1 AND task_id = ?2 AND state IN (?4, ?5) "
        "AND NOT EXISTS (SELECT 1 FROM task_dependency d "
        "JOIN plan_task dependency ON dependency.plan_id = d.plan_id "
        "AND dependency.task_id = d.depends_on_task_id "
        "WHERE d.plan_id = ?1 AND d.task_id = ?2 "
        "AND dependency.state != ?6);");
    if (statement.result() != SQLITE_OK) {
        return SqliteError(connection_->native_handle(), "prepare ready update");
    }
    status = BindText(statement.get(), 1, plan_id);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 2, task_id);
    if (!status.ok()) return status;
    if (sqlite3_bind_int(statement.get(), 3, static_cast<int>(TaskState::kReady))
            != SQLITE_OK
        || sqlite3_bind_int(statement.get(), 4, static_cast<int>(TaskState::kPlanned))
            != SQLITE_OK
        || sqlite3_bind_int(statement.get(), 5, static_cast<int>(TaskState::kRetryable))
            != SQLITE_OK
        || sqlite3_bind_int(statement.get(), 6, static_cast<int>(TaskState::kSucceeded))
            != SQLITE_OK) {
        return Status(StatusCode::kInternal, "SQLite failed to bind ready update");
    }
    const int step = sqlite3_step(statement.get());
    if (step != SQLITE_DONE) {
        return SqliteError(connection_->native_handle(), "set task ready");
    }
    if (sqlite3_changes(connection_->native_handle()) == 0) {
        return Invalid("task is not eligible for READY state");
    }
    return Status::Ok();
}

StatusOr<ClaimedTask> TaskRuntimeRepository::ClaimNextReady(
    const std::string& plan_id,
    ExecutionEpoch epoch,
    const std::string& attempt_id)
{
    Status status = CheckIds(plan_id, "claim");
    if (!status.ok()) return status;
    status = CheckEpochAttempt(epoch, attempt_id);
    if (!status.ok()) return status;
    status = connection_->Execute("BEGIN IMMEDIATE;");
    if (!status.ok()) return status;
    const auto current = ReadCurrentEpochForPlan(
        connection_->native_handle(), plan_id);
    if (!current.ok()) return Rollback(*connection_, current.status());
    if (current.value() != epoch) {
        return Rollback(*connection_, Invalid(
            "stale executor epoch cannot claim a ready task"));
    }

    Statement select(
        connection_->native_handle(),
        "SELECT task_id FROM plan_task pt "
        "WHERE pt.plan_id = ?1 AND pt.state = ?2 "
        "AND NOT EXISTS (SELECT 1 FROM task_dependency d "
        "JOIN plan_task dependency ON dependency.plan_id = d.plan_id "
        "AND dependency.task_id = d.depends_on_task_id "
        "WHERE d.plan_id = pt.plan_id AND d.task_id = pt.task_id "
        "AND dependency.state != ?3) "
        "ORDER BY pt.task_id LIMIT 1;");
    if (select.result() != SQLITE_OK) {
        return Rollback(*connection_, SqliteError(
            connection_->native_handle(), "prepare ready task query"));
    }
    status = BindText(select.get(), 1, plan_id);
    if (!status.ok()) return Rollback(*connection_, status);
    if (sqlite3_bind_int(select.get(), 2, static_cast<int>(TaskState::kReady))
            != SQLITE_OK
        || sqlite3_bind_int(select.get(), 3, static_cast<int>(TaskState::kSucceeded))
            != SQLITE_OK) {
        return Rollback(*connection_, Status(
            StatusCode::kInternal,
            "SQLite failed to bind ready query"));
    }
    const int selected = sqlite3_step(select.get());
    if (selected == SQLITE_DONE) {
        return Rollback(*connection_, Status(
            StatusCode::kNotFound,
            "no eligible ready task exists"));
    }
    if (selected != SQLITE_ROW) {
        return Rollback(*connection_, SqliteError(
            connection_->native_handle(), "select ready task"));
    }
    const TaskId task_id = reinterpret_cast<const char*>(
        sqlite3_column_text(select.get(), 0));

    Statement update(
        connection_->native_handle(),
        "UPDATE plan_task SET state = ?3, owner_epoch = ?4, "
        "active_attempt_id = ?5, attempt_count = attempt_count + 1, "
        "last_error_code = NULL, last_error_message = NULL "
        "WHERE plan_id = ?1 AND task_id = ?2 AND state = ?6 "
        "AND active_attempt_id IS NULL;");
    if (update.result() != SQLITE_OK) {
        return Rollback(*connection_, SqliteError(
            connection_->native_handle(), "prepare task claim"));
    }
    status = BindText(update.get(), 1, plan_id);
    if (!status.ok()) return Rollback(*connection_, status);
    status = BindText(update.get(), 2, task_id);
    if (!status.ok()) return Rollback(*connection_, status);
    if (sqlite3_bind_int(update.get(), 3, static_cast<int>(TaskState::kRunning))
            != SQLITE_OK
        || sqlite3_bind_int(update.get(), 6, static_cast<int>(TaskState::kReady))
            != SQLITE_OK) {
        return Rollback(*connection_, Status(
            StatusCode::kInternal,
            "SQLite failed to bind task claim state"));
    }
    status = BindEpoch(update.get(), 4, epoch);
    if (!status.ok()) return Rollback(*connection_, status);
    status = BindText(update.get(), 5, attempt_id);
    if (!status.ok()) return Rollback(*connection_, status);
    const int updated = sqlite3_step(update.get());
    if (updated != SQLITE_DONE) {
        return Rollback(*connection_, SqliteError(
            connection_->native_handle(), "claim ready task"));
    }
    if (sqlite3_changes(connection_->native_handle()) != 1) {
        return Rollback(*connection_, Invalid("ready task claim was lost"));
    }
    Statement attempt(
        connection_->native_handle(),
        "INSERT INTO task_attempt("
        "attempt_id, plan_id, task_id, owner_epoch, file_state, started_at_ns) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6);");
    if (attempt.result() != SQLITE_OK) {
        return Rollback(*connection_, SqliteError(
            connection_->native_handle(), "prepare task attempt insert"));
    }
    status = BindText(attempt.get(), 1, attempt_id);
    if (!status.ok()) return Rollback(*connection_, status);
    status = BindText(attempt.get(), 2, plan_id);
    if (!status.ok()) return Rollback(*connection_, status);
    status = BindText(attempt.get(), 3, task_id);
    if (!status.ok()) return Rollback(*connection_, status);
    status = BindEpoch(attempt.get(), 4, epoch);
    if (!status.ok()) return Rollback(*connection_, status);
    if (sqlite3_bind_int(
            attempt.get(), 5, static_cast<int>(FileAttemptState::kRunning))
            != SQLITE_OK
        || sqlite3_bind_int64(
               attempt.get(), 6, CurrentTimeNanoseconds()) != SQLITE_OK) {
        return Rollback(*connection_, Status(
            StatusCode::kInternal,
            "SQLite failed to bind task attempt lifecycle fields"));
    }
    if (sqlite3_step(attempt.get()) != SQLITE_DONE) {
        return Rollback(*connection_, SqliteError(
            connection_->native_handle(), "insert task attempt"));
    }
    status = AppendTaskEvent(
        *connection_,
        plan_id,
        task_id,
        "RUNNING",
        epoch,
        attempt_id,
        "claim");
    if (!status.ok()) return Rollback(*connection_, status);
    auto runtime = ReadRuntimeWithStatement(
        connection_->native_handle(), plan_id, task_id);
    if (!runtime.ok()) return Rollback(*connection_, runtime.status());
    status = connection_->Execute("COMMIT;");
    if (!status.ok()) {
        connection_->Execute("ROLLBACK;");
        return status;
    }
    return ClaimedTask{task_id, std::move(runtime.value())};
}

Status TaskRuntimeRepository::MarkSucceeded(
    const std::string& plan_id,
    const TaskId& task_id,
    ExecutionEpoch epoch,
    const std::string& attempt_id)
{
    Status status = CheckIds(plan_id, task_id);
    if (!status.ok()) return status;
    status = CheckEpochAttempt(epoch, attempt_id);
    if (!status.ok()) return status;
    return FinishTask(
        *connection_,
        plan_id,
        task_id,
        epoch,
        attempt_id,
        TaskState::kSucceeded,
        nullptr);
}

Status TaskRuntimeRepository::MarkRetryable(
    const std::string& plan_id,
    const TaskId& task_id,
    ExecutionEpoch epoch,
    const std::string& attempt_id,
    const Status& error)
{
    Status status = CheckIds(plan_id, task_id);
    if (!status.ok()) return status;
    status = CheckEpochAttempt(epoch, attempt_id);
    if (!status.ok()) return status;
    if (error.ok()) return Invalid("retryable task result requires an error");
    return FinishTask(
        *connection_,
        plan_id,
        task_id,
        epoch,
        attempt_id,
        TaskState::kRetryable,
        &error);
}

Status TaskRuntimeRepository::RecoverSucceeded(
    const std::string& plan_id,
    const TaskId& task_id,
    ExecutionEpoch recovery_epoch,
    ExecutionEpoch expected_old_epoch,
    const std::string& expected_old_attempt_id,
    const std::string& reason)
{
    Status status = CheckIds(plan_id, task_id);
    if (!status.ok()) return status;
    status = CheckEpochAttempt(recovery_epoch, expected_old_attempt_id);
    if (!status.ok()) return status;
    status = CheckEpochAttempt(expected_old_epoch, expected_old_attempt_id);
    if (!status.ok()) return status;
    if (reason.empty()) return Invalid("recovery reason must not be empty");
    return RecoverTask(
        *connection_,
        plan_id,
        task_id,
        recovery_epoch,
        expected_old_epoch,
        expected_old_attempt_id,
        TaskState::kSucceeded,
        FileAttemptState::kCommitted,
        "RECOVER_SUCCEEDED",
        reason,
        nullptr);
}

Status TaskRuntimeRepository::RecoverRetryable(
    const std::string& plan_id,
    const TaskId& task_id,
    ExecutionEpoch recovery_epoch,
    ExecutionEpoch expected_old_epoch,
    const std::string& expected_old_attempt_id,
    const std::string& reason)
{
    Status status = CheckIds(plan_id, task_id);
    if (!status.ok()) return status;
    status = CheckEpochAttempt(recovery_epoch, expected_old_attempt_id);
    if (!status.ok()) return status;
    status = CheckEpochAttempt(expected_old_epoch, expected_old_attempt_id);
    if (!status.ok()) return status;
    if (reason.empty()) return Invalid("recovery reason must not be empty");
    const Status error(StatusCode::kIoError, reason);
    return RecoverTask(
        *connection_,
        plan_id,
        task_id,
        recovery_epoch,
        expected_old_epoch,
        expected_old_attempt_id,
        TaskState::kRetryable,
        FileAttemptState::kRetryable,
        "RECOVER_RETRYABLE",
        reason,
        &error);
}

Status TaskRuntimeRepository::RecoverInconsistent(
    const std::string& plan_id,
    const TaskId& task_id,
    ExecutionEpoch recovery_epoch,
    ExecutionEpoch expected_old_epoch,
    const std::string& expected_old_attempt_id,
    const std::string& reason)
{
    Status status = CheckIds(plan_id, task_id);
    if (!status.ok()) return status;
    status = CheckEpochAttempt(recovery_epoch, expected_old_attempt_id);
    if (!status.ok()) return status;
    status = CheckEpochAttempt(expected_old_epoch, expected_old_attempt_id);
    if (!status.ok()) return status;
    if (reason.empty()) return Invalid("recovery reason must not be empty");
    const Status error(StatusCode::kInternal, reason);
    return RecoverTask(
        *connection_,
        plan_id,
        task_id,
        recovery_epoch,
        expected_old_epoch,
        expected_old_attempt_id,
        TaskState::kInconsistent,
        FileAttemptState::kInconsistent,
        "RECOVER_INCONSISTENT",
        reason,
        &error);
}

Status TaskRuntimeRepository::PersistVerifiedReceipt(
    const std::string& plan_id,
    const VerifiedReceipt& receipt)
{
    Status status = CheckIds(plan_id, receipt.task_id);
    if (!status.ok()) return status;
    status = CheckEpochAttempt(receipt.owner_epoch, receipt.attempt_id);
    if (!status.ok()) return status;
    if (receipt.temp_path.empty() || receipt.final_path.empty()
        || !receipt.source_digest.has_value()
        || !receipt.source_identity.has_value()) {
        return Invalid("verified receipt is missing required fields");
    }
    if (receipt.content_size
        > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return Invalid("verified receipt size exceeds SQLite integer range");
    }

    const auto identity = EncodeIdentity(receipt.source_identity.value());
    status = connection_->Execute("BEGIN IMMEDIATE;");
    if (!status.ok()) return status;
    const auto current = ReadCurrentEpochForPlan(
        connection_->native_handle(), plan_id);
    if (!current.ok()) return Rollback(*connection_, current.status());
    if (current.value() != receipt.owner_epoch) {
        return Rollback(*connection_, Invalid(
            "stale executor epoch cannot persist a verified receipt"));
    }
    status = CheckTaskOwnership(
        connection_->native_handle(),
        plan_id,
        receipt.task_id,
        receipt.owner_epoch,
        receipt.attempt_id);
    if (!status.ok()) return Rollback(*connection_, status);
    Statement statement(
        connection_->native_handle(),
        "INSERT INTO verified_receipt("
        "plan_id, task_id, attempt_id, owner_epoch, temp_path, final_path, "
        "content_size, source_digest, target_digest, source_identity) "
        "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10) "
        "ON CONFLICT(plan_id, task_id, attempt_id) DO NOTHING;");
    if (statement.result() != SQLITE_OK) {
        return Rollback(*connection_, SqliteError(
            connection_->native_handle(), "prepare verified receipt insert"));
    }
    status = BindText(statement.get(), 1, plan_id);
    if (!status.ok()) return Rollback(*connection_, status);
    status = BindText(statement.get(), 2, receipt.task_id);
    if (!status.ok()) return Rollback(*connection_, status);
    status = BindText(statement.get(), 3, receipt.attempt_id);
    if (!status.ok()) return Rollback(*connection_, status);
    status = BindEpoch(statement.get(), 4, receipt.owner_epoch);
    if (!status.ok()) return Rollback(*connection_, status);
    status = BindBlob(
        statement.get(), 5, receipt.temp_path.data(), receipt.temp_path.size());
    if (!status.ok()) return Rollback(*connection_, status);
    status = BindBlob(
        statement.get(), 6, receipt.final_path.data(), receipt.final_path.size());
    if (!status.ok()) return Rollback(*connection_, status);
    if (sqlite3_bind_int64(
            statement.get(), 7, static_cast<sqlite3_int64>(receipt.content_size))
        != SQLITE_OK) {
        return Rollback(*connection_, Status(
            StatusCode::kInternal, "SQLite failed to bind receipt size"));
    }
    status = BindBlob(
        statement.get(),
        8,
        receipt.source_digest->bytes.data(),
        receipt.source_digest->bytes.size());
    if (!status.ok()) return Rollback(*connection_, status);
    status = BindBlob(
        statement.get(),
        9,
        receipt.target_digest.bytes.data(),
        receipt.target_digest.bytes.size());
    if (!status.ok()) return Rollback(*connection_, status);
    status = BindBlob(
        statement.get(), 10, identity.data(), identity.size());
    if (!status.ok()) return Rollback(*connection_, status);
    if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        return Rollback(*connection_, SqliteError(
            connection_->native_handle(), "insert verified receipt"));
    }
    const int changes = sqlite3_changes(connection_->native_handle());
    if (changes == 0) {
        const auto existing = ReadVerifiedReceipt(
            plan_id, receipt.task_id, receipt.attempt_id);
        if (!existing.ok()) {
            return Rollback(*connection_, existing.status());
        }
        if (!SameVerifiedReceipt(existing.value(), receipt)) {
            return Rollback(*connection_, Status(
                StatusCode::kInternal,
                "verified receipt conflicts with existing evidence"));
        }
        status = connection_->Execute("COMMIT;");
        if (!status.ok()) connection_->Execute("ROLLBACK;");
        return status;
    }
    if (changes != 1) {
        return Rollback(*connection_, Status(
            StatusCode::kInternal,
            "verified receipt insert changed an unexpected number of rows"));
    }
    status = UpdateAttemptState(
        *connection_,
        plan_id,
        receipt.task_id,
        receipt.attempt_id,
        receipt.owner_epoch,
        FileAttemptState::kVerifiedDurable,
        nullptr,
        false);
    if (!status.ok()) return Rollback(*connection_, status);
    status = AppendTaskEvent(
        *connection_,
        plan_id,
        receipt.task_id,
        "VERIFIED_DURABLE",
        receipt.owner_epoch,
        receipt.attempt_id,
        "receipt persisted before publish");
    if (!status.ok()) return Rollback(*connection_, status);
    status = connection_->Execute("COMMIT;");
    if (!status.ok()) connection_->Execute("ROLLBACK;");
    return status;
}

StatusOr<VerifiedReceipt> TaskRuntimeRepository::ReadVerifiedReceipt(
    const std::string& plan_id,
    const TaskId& task_id,
    const std::string& attempt_id) const
{
    Status status = CheckIds(plan_id, task_id);
    if (!status.ok()) return status;
    if (attempt_id.empty() || attempt_id.find('\0') != std::string::npos) {
        return Invalid("attempt id must be non-empty and NUL-free");
    }
    Statement statement(
        connection_->native_handle(),
        "SELECT task_id, attempt_id, owner_epoch, temp_path, final_path, "
        "content_size, source_digest, target_digest, source_identity "
        "FROM verified_receipt WHERE plan_id = ?1 AND task_id = ?2 "
        "AND attempt_id = ?3;");
    if (statement.result() != SQLITE_OK) {
        return SqliteError(
            connection_->native_handle(), "prepare verified receipt query");
    }
    status = BindText(statement.get(), 1, plan_id);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 2, task_id);
    if (!status.ok()) return status;
    status = BindText(statement.get(), 3, attempt_id);
    if (!status.ok()) return status;
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return Status(StatusCode::kNotFound, "verified receipt was not found");
    }
    if (step != SQLITE_ROW) {
        return SqliteError(
            connection_->native_handle(), "read verified receipt");
    }
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_TEXT
        || sqlite3_column_type(statement.get(), 1) != SQLITE_TEXT
        || sqlite3_column_type(statement.get(), 2) != SQLITE_INTEGER
        || sqlite3_column_type(statement.get(), 3) != SQLITE_BLOB
        || sqlite3_column_type(statement.get(), 4) != SQLITE_BLOB
        || sqlite3_column_type(statement.get(), 5) != SQLITE_INTEGER
        || sqlite3_column_type(statement.get(), 6) != SQLITE_BLOB
        || sqlite3_column_type(statement.get(), 7) != SQLITE_BLOB
        || sqlite3_column_type(statement.get(), 8) != SQLITE_BLOB) {
        return Status(StatusCode::kInternal, "verified receipt has invalid columns");
    }
    const sqlite3_int64 owner_epoch = sqlite3_column_int64(statement.get(), 2);
    const sqlite3_int64 content_size = sqlite3_column_int64(statement.get(), 5);
    if (owner_epoch <= 0 || content_size < 0) {
        return Status(
            StatusCode::kInternal,
            "verified receipt has an invalid epoch or content size");
    }
    const auto read_blob = [&statement](int index) {
        return std::string(
            static_cast<const char*>(sqlite3_column_blob(statement.get(), index)),
            static_cast<std::size_t>(sqlite3_column_bytes(statement.get(), index)));
    };
    const std::string source_digest = read_blob(6);
    const std::string target_digest = read_blob(7);
    if (source_digest.size() != 32 || target_digest.size() != 32) {
        return Status(StatusCode::kInternal, "verified receipt has invalid digest");
    }
    VerifiedReceipt receipt;
    receipt.task_id = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
    receipt.attempt_id = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1));
    receipt.owner_epoch.value = static_cast<std::uint64_t>(owner_epoch);
    receipt.temp_path = read_blob(3);
    receipt.final_path = read_blob(4);
    receipt.content_size = static_cast<std::uint64_t>(content_size);
    std::memcpy(receipt.source_digest.emplace().bytes.data(), source_digest.data(), 32);
    std::memcpy(receipt.target_digest.bytes.data(), target_digest.data(), 32);
    auto identity = DecodeIdentity(
        sqlite3_column_blob(statement.get(), 8),
        sqlite3_column_bytes(statement.get(), 8));
    if (!identity.ok()) return identity.status();
    receipt.source_identity = identity.value();
    return receipt;
}

StatusOr<TaskRuntime> TaskRuntimeRepository::ReadRuntime(
    const std::string& plan_id,
    const TaskId& task_id) const
{
    Status status = CheckIds(plan_id, task_id);
    if (!status.ok()) return status;
    return ReadRuntimeWithStatement(
        connection_->native_handle(), plan_id, task_id);
}

}  // namespace photobridge
