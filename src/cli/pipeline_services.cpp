#include "photobridge/cli/pipeline_services.h"

#include <sqlite3.h>
#include <blake3.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cstddef>
#include <span>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "photobridge/app/manifest_builder.h"
#include "photobridge/app/migration_attempt_preparer.h"
#include "photobridge/app/plan_execution_lock.h"
#include "photobridge/app/sqlite_connection.h"
#include "photobridge/app/sqlite_schema.h"
#include "photobridge/app/task_runtime_repository.h"
#include "photobridge/app/workspace_layout.h"
#include "photobridge/common/posix_error.h"
#include "photobridge/common/test_hooks.h"
#include "photobridge/common/time.h"
#include "photobridge/filesystem/binary_verifier.h"
#include "photobridge/filesystem/copy_and_hash.h"
#include "photobridge/filesystem/linux_file_ops.h"
#include "photobridge/filesystem/mutation_guard.h"
#include "photobridge/model/audit_report.h"
#include "photobridge/model/canonical_plan.h"
#include "photobridge/model/capability.h"
#include "photobridge/model/diff_engine.h"
#include "photobridge/model/loss_analysis.h"
#include "photobridge/model/logical_asset.h"
#include "photobridge/model/path_mapper.h"
#include "photobridge/model/plan_artifact.h"
#include "photobridge/source/local_folder_source.h"

namespace photobridge {
namespace {

bool RequiresInput(PipelineStage)
{
    return true;
}

Status SqliteReadError(
    sqlite3* database,
    std::string_view operation)
{
    return Status(
        StatusCode::kIoError,
        std::string(operation) + ": " + sqlite3_errmsg(database));
}

StatusOr<std::uint64_t> ReadUnsignedInteger(
    sqlite3_stmt* statement,
    int column,
    std::string_view field_name)
{
    if (sqlite3_column_type(statement, column) != SQLITE_INTEGER) {
        return Status(
            StatusCode::kInternal,
            std::string("manifest field is not an integer: ")
                + std::string(field_name));
    }

    const sqlite3_int64 value = sqlite3_column_int64(statement, column);
    if (value < 0) {
        return Status(
            StatusCode::kInternal,
            std::string("manifest field is negative: ")
                + std::string(field_name));
    }
    return static_cast<std::uint64_t>(value);
}

StatusOr<std::vector<PhysicalAsset>> ReadManifestAssets(
    SqliteConnection& connection,
    std::string_view manifest_id)
{
    sqlite3_stmt* statement = nullptr;
    const int prepare_result = sqlite3_prepare_v2(
        connection.native_handle(),
        "SELECT relative_path, device, inode, size, mtime_ns, ctime_ns, "
        "kind FROM physical_asset WHERE manifest_id = ? "
        "ORDER BY relative_path ASC;",
        -1,
        &statement,
        nullptr);
    if (prepare_result != SQLITE_OK) {
        return SqliteReadError(
            connection.native_handle(),
            "prepare manifest asset read");
    }

    const int bind_result = sqlite3_bind_text(
        statement,
        1,
        manifest_id.data(),
        static_cast<int>(manifest_id.size()),
        SQLITE_TRANSIENT);
    if (bind_result != SQLITE_OK) {
        sqlite3_finalize(statement);
        return SqliteReadError(
            connection.native_handle(),
            "bind manifest id for asset read");
    }

    std::vector<PhysicalAsset> assets;
    while (true) {
        const int step_result = sqlite3_step(statement);
        if (step_result == SQLITE_DONE) {
            break;
        }
        if (step_result != SQLITE_ROW) {
            const Status status = SqliteReadError(
                connection.native_handle(),
                "read manifest asset");
            sqlite3_finalize(statement);
            return status;
        }

        if (sqlite3_column_type(statement, 0) != SQLITE_BLOB) {
            sqlite3_finalize(statement);
            return Status(
                StatusCode::kInternal,
                "manifest relative path is not a blob");
        }
        const int path_size = sqlite3_column_bytes(statement, 0);
        const void* path_data = sqlite3_column_blob(statement, 0);
        if (path_size <= 0 || path_data == nullptr) {
            sqlite3_finalize(statement);
            return Status(
                StatusCode::kInternal,
                "manifest relative path is empty");
        }
        auto relative_path = RelativePath::Parse(
            std::string(
                static_cast<const char*>(path_data),
                static_cast<std::size_t>(path_size)));
        if (!relative_path.ok()) {
            sqlite3_finalize(statement);
            return relative_path.status();
        }

        auto device = ReadUnsignedInteger(statement, 1, "device");
        auto inode = ReadUnsignedInteger(statement, 2, "inode");
        auto size = ReadUnsignedInteger(statement, 3, "size");
        if (!device.ok() || !inode.ok() || !size.ok()) {
            const Status status = !device.ok()
                ? device.status()
                : (!inode.ok() ? inode.status() : size.status());
            sqlite3_finalize(statement);
            return status;
        }

        if (sqlite3_column_type(statement, 4) != SQLITE_INTEGER
            || sqlite3_column_type(statement, 5) != SQLITE_INTEGER
            || sqlite3_column_type(statement, 6) != SQLITE_INTEGER) {
            sqlite3_finalize(statement);
            return Status(
                StatusCode::kInternal,
                "manifest identity or kind field is not an integer");
        }

        const sqlite3_int64 kind = sqlite3_column_int64(statement, 6);
        if (kind < 0 || kind > static_cast<sqlite3_int64>(
                AssetKind::kUnknown)) {
            sqlite3_finalize(statement);
            return Status(
                StatusCode::kInternal,
                "manifest asset kind is unknown");
        }

        FileIdentity identity;
        identity.device = device.value();
        identity.inode = inode.value();
        identity.size = size.value();
        identity.mtime_ns = sqlite3_column_int64(statement, 4);
        identity.ctime_ns = sqlite3_column_int64(statement, 5);
        assets.push_back(PhysicalAsset{
            std::move(relative_path.value()),
            identity,
            static_cast<AssetKind>(kind),
            {},
        });
    }

    sqlite3_finalize(statement);
    return assets;
}

Status WritePlanFile(
    const std::filesystem::path& path,
    std::string_view bytes)
{
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        return Status(
            StatusCode::kIoError,
            "check plan artifact path: " + exists_error.message());
    }

    if (exists) {
        if (std::filesystem::is_directory(path, exists_error)) {
            return Status(
                StatusCode::kAlreadyExists,
                "plan artifact path is a directory: " + path.string());
        }
        if (exists_error) {
            return Status(
                StatusCode::kIoError,
                "inspect plan artifact path: " + exists_error.message());
        }

        std::ifstream existing(path, std::ios::binary);
        if (!existing) {
            return Status(
                StatusCode::kIoError,
                "open existing plan artifact: " + path.string());
        }
        const std::string existing_bytes{
            std::istreambuf_iterator<char>(existing),
            std::istreambuf_iterator<char>()};
        if (std::string_view(existing_bytes) == bytes) {
            return Status::Ok();
        }
        return Status(
            StatusCode::kAlreadyExists,
            "plan artifact already exists with different bytes: "
                + path.string());
    }

    LinuxFileOps file_ops;
    auto parent = file_ops.OpenRoot(path.parent_path(), OpenRootMode::kExisting);
    if (!parent.ok()) return parent.status();
    const std::string final_name = path.filename().string();
    const std::string temp_name = ".pbtmp." + final_name;
    auto temporary = file_ops.CreateTempNoReplace(
        parent.value().get(),
        temp_name,
        0600);
    if (!temporary.ok()) return temporary.status();

    const auto cleanup = [&file_ops, &parent, &temp_name]() {
        static_cast<void>(file_ops.UnlinkAt(parent.value().get(), temp_name));
    };
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto* data = reinterpret_cast<const std::byte*>(
            bytes.data() + offset);
        const std::size_t remaining = bytes.size() - offset;
        auto written = file_ops.Write(
            temporary.value().get(),
            std::span<const std::byte>(data, remaining));
        if (!written.ok()) {
            cleanup();
            return written.status();
        }
        if (written.value() == 0) {
            cleanup();
            return Status(
                StatusCode::kIoError,
                "plan artifact write made no progress");
        }
        offset += written.value();
    }
    Status status = file_ops.Fdatasync(temporary.value().get());
    if (!status.ok()) {
        cleanup();
        return status;
    }
    status = file_ops.RenameNoReplace(
        parent.value().get(),
        temp_name,
        parent.value().get(),
        final_name);
    if (!status.ok()) return status;
    return file_ops.FsyncDirectory(parent.value().get());
}

StatusOr<std::string> ReadPlanFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Status(
            StatusCode::kNotFound,
            "open frozen plan: " + path.string());
    }

    std::string bytes{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (input.bad()) {
        return Status(
            StatusCode::kIoError,
            "read frozen plan: " + path.string());
    }
    return bytes;
}

class PipelineStatement final {
public:
    PipelineStatement(sqlite3* database, const char* sql) noexcept
    {
        result_ = sqlite3_prepare_v2(
            database,
            sql,
            -1,
            &statement_,
            nullptr);
    }

    ~PipelineStatement()
    {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    PipelineStatement(const PipelineStatement&) = delete;
    PipelineStatement& operator=(const PipelineStatement&) = delete;

    int result() const noexcept { return result_; }
    sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3_stmt* statement_ = nullptr;
    int result_ = SQLITE_ERROR;
};

StatusOr<std::array<std::uint64_t, 9>> ReadTaskStateCounts(
    SqliteConnection& connection,
    std::string_view plan_id)
{
    PipelineStatement statement(
        connection.native_handle(),
        "SELECT state, COUNT(*) FROM plan_task "
        "WHERE plan_id = ?1 GROUP BY state ORDER BY state;");
    if (statement.result() != SQLITE_OK) {
        return SqliteReadError(
            connection.native_handle(),
            "prepare task state summary");
    }
    if (sqlite3_bind_text(
            statement.get(),
            1,
            plan_id.data(),
            static_cast<int>(plan_id.size()),
            SQLITE_TRANSIENT)
        != SQLITE_OK) {
        return SqliteReadError(
            connection.native_handle(),
            "bind plan id for task state summary");
    }

    std::array<std::uint64_t, 9> counts{};
    while (true) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE) {
            break;
        }
        if (step != SQLITE_ROW
            || sqlite3_column_type(statement.get(), 0) != SQLITE_INTEGER
            || sqlite3_column_type(statement.get(), 1) != SQLITE_INTEGER) {
            return SqliteReadError(
                connection.native_handle(),
                "read task state summary");
        }
        const sqlite3_int64 state = sqlite3_column_int64(statement.get(), 0);
        const sqlite3_int64 count = sqlite3_column_int64(statement.get(), 1);
        if (state < 0 || state >= static_cast<sqlite3_int64>(counts.size())
            || count < 0) {
            return Status(
                StatusCode::kInternal,
                "SQLite task state summary is out of range");
        }
        counts[static_cast<std::size_t>(state)] =
            static_cast<std::uint64_t>(count);
    }
    return counts;
}

StatusOr<std::string> ReadManifestSourceRoot(
    SqliteConnection& connection,
    std::string_view manifest_id)
{
    PipelineStatement statement(
        connection.native_handle(),
        "SELECT source_root FROM source_manifest "
        "WHERE manifest_id = ?1;");
    if (statement.result() != SQLITE_OK) {
        return SqliteReadError(
            connection.native_handle(),
            "prepare manifest source root read");
    }
    if (sqlite3_bind_text(
            statement.get(),
            1,
            manifest_id.data(),
            static_cast<int>(manifest_id.size()),
            SQLITE_TRANSIENT)
        != SQLITE_OK) {
        return SqliteReadError(
            connection.native_handle(),
            "bind manifest id for source root read");
    }

    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return Status(
            StatusCode::kNotFound,
            "source manifest was not found: " + std::string(manifest_id));
    }
    if (step != SQLITE_ROW) {
        return SqliteReadError(
            connection.native_handle(),
            "read manifest source root");
    }
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_BLOB) {
        return Status(
            StatusCode::kInternal,
            "manifest source root is not a blob");
    }
    const int size = sqlite3_column_bytes(statement.get(), 0);
    const void* data = sqlite3_column_blob(statement.get(), 0);
    if (size <= 0 || data == nullptr) {
        return Status(
            StatusCode::kInternal,
            "manifest source root is empty");
    }
    return std::string(
        static_cast<const char*>(data),
        static_cast<std::size_t>(size));
}

Status BindPipelineText(
    sqlite3_stmt* statement,
    int index,
    std::string_view value)
{
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Status(
            StatusCode::kInvalidArgument,
            "SQLite text value is too large");
    }
    if (sqlite3_bind_text(
            statement,
            index,
            value.data(),
            static_cast<int>(value.size()),
            SQLITE_TRANSIENT)
        != SQLITE_OK) {
        return Status(
            StatusCode::kInternal,
            "SQLite failed to bind text value");
    }
    return Status::Ok();
}

Status BindPipelineBlob(
    sqlite3_stmt* statement,
    int index,
    const void* data,
    std::size_t size)
{
    if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return Status(
            StatusCode::kInvalidArgument,
            "SQLite blob value is too large");
    }
    if (sqlite3_bind_blob(
            statement,
            index,
            data,
            static_cast<int>(size),
            SQLITE_TRANSIENT)
        != SQLITE_OK) {
        return Status(
            StatusCode::kInternal,
            "SQLite failed to bind blob value");
    }
    return Status::Ok();
}

Status RollbackMaterialization(SqliteConnection& connection, Status status)
{
    connection.Execute("ROLLBACK;");
    return status;
}

StatusOr<TaskSpec> TaskSpecForPlanAsset(const MinimalPlanAsset& asset)
{
    TaskSpec task{
        {},
        {},
        TaskType::kMigrateFile,
        asset.logical_asset_id,
        asset.source_asset_id,
        asset.target_path,
        asset.source_identity.size,
        std::nullopt,
    };
    auto task_key = TaskKeyFor(task);
    if (!task_key.ok()) {
        return task_key.status();
    }
    task.task_key = task_key.value();
    task.id = task.task_key;
    return task;
}

std::string TempNameFor(
    std::string_view plan_id,
    std::string_view task_id,
    std::string_view attempt_id)
{
    std::string payload = "photobridge-temp-v1\n";
    const auto append = [&payload](std::string_view value) {
        payload += std::to_string(value.size());
        payload.push_back(':');
        payload.append(value.data(), value.size());
    };
    append(plan_id);
    append(task_id);
    append(attempt_id);

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, payload.data(), payload.size());
    Digest digest;
    blake3_hasher_finalize(
        &hasher,
        reinterpret_cast<std::uint8_t*>(digest.bytes.data()),
        digest.bytes.size());
    return ".photobridge." + digest.ToHex() + ".pbtmp";
}

StatusOr<bool> ObserveRecoveryFile(
    LinuxFileOps& file_ops,
    int root_fd,
    const std::string& path_bytes,
    std::uint64_t expected_size,
    const Digest& expected_digest,
    bool temp,
    ObservedFileState& observed,
    std::span<std::byte> buffer)
{
    auto path = RelativePath::Parse(path_bytes);
    if (!path.ok()) return path.status();
    auto file = file_ops.OpenSource(root_fd, path.value());
    if (!file.ok()) {
        if (file.status().code() == StatusCode::kNotFound) return false;
        return file.status();
    }

    Blake3Hasher hasher;
    auto verification = VerifyBinary(
        file_ops,
        hasher,
        file.value().get(),
        expected_size,
        std::optional<Digest>(expected_digest),
        buffer);
    if (!verification.ok()) return verification.status();
    if (temp) {
        observed.temp_exists = true;
        observed.temp_size = verification.value().bytes_read;
        observed.temp_digest = verification.value().target_digest;
        observed.temp_identity = verification.value().target_after;
    } else {
        observed.final_exists = true;
        observed.final_size = verification.value().bytes_read;
        observed.final_digest = verification.value().target_digest;
        observed.final_identity = verification.value().target_after;
    }
    return true;
}

std::string TaskStateName(TaskState state)
{
    switch (state) {
    case TaskState::kPlanned:
        return "PLANNED";
    case TaskState::kReady:
        return "READY";
    case TaskState::kRunning:
        return "RUNNING";
    case TaskState::kSucceeded:
        return "SUCCEEDED";
    case TaskState::kRetryable:
        return "RETRYABLE";
    case TaskState::kFailed:
        return "FAILED";
    case TaskState::kNeedsReview:
        return "NEEDS_REVIEW";
    case TaskState::kInconsistent:
        return "INCONSISTENT";
    case TaskState::kSkipped:
        return "SKIPPED";
    }
    return "UNKNOWN";
}

const char* ReconcileActionName(ReconcileAction action)
{
    switch (action) {
    case ReconcileAction::kRetryTask: return "retry_task";
    case ReconcileAction::kResumeCommitFromTemp: return "resume_commit";
    case ReconcileAction::kAdoptFinal: return "adopt_final";
    case ReconcileAction::kAdoptFinalAndCleanupTemp:
        return "adopt_final_cleanup_temp";
    case ReconcileAction::kReverifyCommitted: return "reverify_committed";
    case ReconcileAction::kTargetConflict: return "target_conflict";
    case ReconcileAction::kInconsistent: return "inconsistent";
    }
    return "unknown";
}

AuditReport RecoveryAudit(
    std::string_view plan_id,
    std::string_view task_id,
    const CommitIntent& intent,
    const std::optional<VerifiedReceipt>& receipt,
    const ObservedFileState& observed,
    const ReconcileDecision& decision)
{
    std::vector<DiffFileState> expected;
    std::vector<DiffFileState> actual;
    if (receipt.has_value()) {
        expected.push_back({
            receipt->temp_path,
            receipt->content_size,
            receipt->target_digest,
        });
        expected.push_back({
            receipt->final_path,
            receipt->content_size,
            receipt->target_digest,
        });
    }
    if (observed.temp_exists) {
        actual.push_back({
            receipt.has_value() ? receipt->temp_path : intent.temp_path,
            observed.temp_size,
            observed.temp_digest,
        });
    }
    if (observed.final_exists) {
        actual.push_back({
            intent.final_path,
            observed.final_size,
            observed.final_digest,
        });
    }
    return AuditReport{
        std::string(plan_id),
        std::string(task_id),
        ReconcileActionName(decision.action),
        decision.reason,
        DiffFileStates(expected, actual),
        {},
    };
}

StatusOr<bool> CheckMaterializedPlan(
    SqliteConnection& connection,
    const FrozenPlanFile& artifact)
{
    PipelineStatement statement(
        connection.native_handle(),
        "SELECT artifact_digest, semantic_digest, format_version "
        "FROM migration_plan WHERE plan_id = ?1;");
    if (statement.result() != SQLITE_OK) {
        return SqliteReadError(
            connection.native_handle(),
            "prepare materialized plan lookup");
    }
    Status status = BindPipelineText(statement.get(), 1, artifact.plan_id);
    if (!status.ok()) return status;

    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE) {
        return false;
    }
    if (step != SQLITE_ROW) {
        return SqliteReadError(
            connection.native_handle(),
            "look up materialized plan");
    }
    if (sqlite3_column_type(statement.get(), 0) != SQLITE_BLOB
        || sqlite3_column_type(statement.get(), 1) != SQLITE_BLOB
        || sqlite3_column_type(statement.get(), 2) != SQLITE_INTEGER
        || sqlite3_column_bytes(statement.get(), 0)
            != static_cast<int>(artifact.artifact_digest.bytes.size())
        || sqlite3_column_bytes(statement.get(), 1)
            != static_cast<int>(artifact.semantic_digest.bytes.size())
        || sqlite3_column_int(statement.get(), 2)
            != static_cast<int>(artifact.plan.format_version())) {
        return Status(
            StatusCode::kAlreadyExists,
            "materialized plan metadata differs: " + artifact.plan_id);
    }
    const auto matches = [&statement](int column, const Digest& expected) {
        return std::equal(
            expected.bytes.begin(),
            expected.bytes.end(),
            static_cast<const std::byte*>(sqlite3_column_blob(
                statement.get(),
                column)));
    };
    if (!matches(0, artifact.artifact_digest)
        || !matches(1, artifact.semantic_digest)) {
        return Status(
            StatusCode::kAlreadyExists,
            "materialized plan digest differs: " + artifact.plan_id);
    }
    return true;
}

Status MaterializePlan(
    SqliteConnection& connection,
    const FrozenPlanFile& artifact,
    const std::filesystem::path& artifact_path,
    std::size_t& task_count)
{
    if (artifact.plan_id.empty()) {
        return Status(
            StatusCode::kInternal,
            "frozen plan has an empty plan id");
    }

    Status status = connection.Execute("BEGIN IMMEDIATE;");
    if (!status.ok()) return status;

    auto materialized = CheckMaterializedPlan(connection, artifact);
    if (!materialized.ok()) {
        return RollbackMaterialization(connection, materialized.status());
    }
    if (materialized.value()) {
        auto counts = ReadTaskStateCounts(connection, artifact.plan_id);
        if (!counts.ok()) {
            return RollbackMaterialization(connection, counts.status());
        }
        task_count = 0;
        for (const std::uint64_t count : counts.value()) {
            task_count += count;
        }
        if (task_count != artifact.plan.assets().size()) {
            return RollbackMaterialization(
                connection,
                Status(
                    StatusCode::kInternal,
                    "materialized plan task count differs from frozen plan"));
        }
        status = connection.Execute("COMMIT;");
        if (!status.ok()) connection.Execute("ROLLBACK;");
        return status;
    }

    const std::string migration_id = "migration-" + artifact.plan_id;
    const std::int64_t created_at_ns = CurrentTimeNanoseconds();

    PipelineStatement migration(
        connection.native_handle(),
        "INSERT INTO migration(migration_id, source_manifest_id, target_root, "
        "state, current_epoch, created_at_ns) VALUES(?1, ?2, ?3, 0, 0, ?4);");
    if (migration.result() != SQLITE_OK) {
        return RollbackMaterialization(
            connection,
            SqliteReadError(
                connection.native_handle(),
                "prepare migration insert"));
    }
    status = BindPipelineText(migration.get(), 1, migration_id);
    if (!status.ok()) return RollbackMaterialization(connection, status);
    status = BindPipelineText(
        migration.get(),
        2,
        artifact.plan.source_manifest_id());
    if (!status.ok()) return RollbackMaterialization(connection, status);
    status = BindPipelineBlob(
        migration.get(),
        3,
        artifact.plan.target_root().data(),
        artifact.plan.target_root().size());
    if (!status.ok()) return RollbackMaterialization(connection, status);
    if (sqlite3_bind_int64(migration.get(), 4, created_at_ns) != SQLITE_OK) {
        return RollbackMaterialization(
            connection,
            Status(
                StatusCode::kInternal,
                "SQLite failed to bind migration timestamp"));
    }
    int step = sqlite3_step(migration.get());
    if (step == SQLITE_CONSTRAINT) {
        return RollbackMaterialization(
            connection,
            Status(
                StatusCode::kAlreadyExists,
                "migration already exists: " + migration_id));
    }
    if (step != SQLITE_DONE) {
        return RollbackMaterialization(
            connection,
            SqliteReadError(connection.native_handle(), "insert migration"));
    }

    PipelineStatement plan(
        connection.native_handle(),
        "INSERT INTO migration_plan(plan_id, migration_id, plan_path, "
        "artifact_digest, semantic_digest, semantic_profile_version, "
        "format_version, state, created_at_ns) "
        "VALUES(?1, ?2, ?3, ?4, ?5, 1, ?6, 0, ?7);");
    if (plan.result() != SQLITE_OK) {
        return RollbackMaterialization(
            connection,
            SqliteReadError(
                connection.native_handle(),
                "prepare migration plan insert"));
    }
    status = BindPipelineText(plan.get(), 1, artifact.plan_id);
    if (!status.ok()) return RollbackMaterialization(connection, status);
    status = BindPipelineText(plan.get(), 2, migration_id);
    if (!status.ok()) return RollbackMaterialization(connection, status);
    const std::string plan_path = artifact_path.string();
    status = BindPipelineBlob(
        plan.get(),
        3,
        plan_path.data(),
        plan_path.size());
    if (!status.ok()) return RollbackMaterialization(connection, status);
    status = BindPipelineBlob(
        plan.get(),
        4,
        artifact.artifact_digest.bytes.data(),
        artifact.artifact_digest.bytes.size());
    if (!status.ok()) return RollbackMaterialization(connection, status);
    status = BindPipelineBlob(
        plan.get(),
        5,
        artifact.semantic_digest.bytes.data(),
        artifact.semantic_digest.bytes.size());
    if (!status.ok()) return RollbackMaterialization(connection, status);
    if (sqlite3_bind_int(
            plan.get(),
            6,
            static_cast<int>(artifact.plan.format_version())) != SQLITE_OK
        || sqlite3_bind_int64(plan.get(), 7, created_at_ns) != SQLITE_OK) {
        return RollbackMaterialization(
            connection,
            Status(
                StatusCode::kInternal,
                "SQLite failed to bind migration plan fields"));
    }
    step = sqlite3_step(plan.get());
    if (step != SQLITE_DONE) {
        return RollbackMaterialization(
            connection,
            SqliteReadError(
                connection.native_handle(),
                "insert migration plan"));
    }

    TaskRuntimeRepository repository(connection);
    task_count = 0;
    for (const MinimalPlanAsset& asset : artifact.plan.assets()) {
        auto task = TaskSpecForPlanAsset(asset);
        if (!task.ok()) {
            return RollbackMaterialization(connection, task.status());
        }

        status = repository.AddTask(artifact.plan_id, task.value());
        if (!status.ok()) {
            return RollbackMaterialization(connection, status);
        }
        status = repository.SetReady(artifact.plan_id, task.value().id);
        if (!status.ok()) {
            return RollbackMaterialization(connection, status);
        }
        ++task_count;
    }

    status = connection.Execute("COMMIT;");
    if (!status.ok()) {
        connection.Execute("ROLLBACK;");
    }
    return status;
}

struct VerificationSummary {
    std::uint64_t bytes = 0;
    DiffFileState expected;
    DiffFileState observed;
};

StatusOr<VerificationSummary> VerifyPlanAsset(
    FileOps& file_ops,
    int source_root_fd,
    int target_root_fd,
    const MinimalPlanAsset& plan_asset,
    std::span<std::byte> buffer)
{
    if (plan_asset.target_path.components().size() != 1) {
        return Status(
            StatusCode::kInvalidArgument,
            "single-thread verification requires a single target path component");
    }

    const PhysicalAsset source_asset{
        plan_asset.source_path,
        plan_asset.source_identity,
        AssetKind::kUnknown,
        {},
    };
    auto source_guard = MutationGuard::Open(
        file_ops,
        source_root_fd,
        source_asset);
    if (!source_guard.ok()) return source_guard.status();
    Status status = source_guard.value().VerifyBeforeRead();
    if (!status.ok()) return status;

    Blake3Hasher source_hasher;
    auto source_verification = VerifyBinary(
        file_ops,
        source_hasher,
        source_guard.value().fd(),
        plan_asset.source_identity.size,
        std::nullopt,
        buffer);
    if (!source_verification.ok()) return source_verification.status();
    status = source_guard.value().VerifyAfterRead();
    if (!status.ok()) return status;
    if (source_verification.value().target_before
            != source_guard.value().manifest_identity()
        || source_verification.value().target_after
            != source_guard.value().manifest_identity()
        || source_verification.value().bytes_read
            != plan_asset.source_identity.size) {
        return Status(
            StatusCode::kInternal,
            "source changed during independent verification");
    }

    auto target_fd = file_ops.OpenSource(target_root_fd, plan_asset.target_path);
    if (!target_fd.ok()) return target_fd.status();
    Blake3Hasher target_hasher;
    auto target_verification = VerifyBinary(
        file_ops,
        target_hasher,
        target_fd.value().get(),
        plan_asset.source_identity.size,
        std::optional<Digest>(source_verification.value().target_digest),
        buffer);
    if (!target_verification.ok()) return target_verification.status();
    return VerificationSummary{
        target_verification.value().bytes_read,
        DiffFileState{
            std::string(plan_asset.target_path.bytes()),
            source_verification.value().bytes_read,
            source_verification.value().target_digest,
        },
        DiffFileState{
            std::string(plan_asset.target_path.bytes()),
            target_verification.value().bytes_read,
            target_verification.value().target_digest,
        },
    };
}

}  // namespace


class ScanService final {
public:
    static Status Execute(
        const StatusOr<WorkspaceLayout>& layout,
        const std::string& input_path,
        CommandContext& context)
    {
        auto connection = SqliteConnection::Open(layout.value().database);
        if (!connection.ok()) {
            return connection.status();
        }

        const Status schema_status = EnsureSchema(connection.value());
        if (!schema_status.ok()) {
            return schema_status;
        }

        LocalFolderSource source{std::filesystem::path(input_path)};
        SqliteManifestBuilder builder(connection.value());
        const Status begin_status = builder.Begin({
            input_path,
            std::string(source.TypeName()),
            input_path,
        });
        if (!begin_status.ok()) {
            return begin_status;
        }

        const Status scan_status = source.Scan(builder);
        if (!scan_status.ok()) {
            return scan_status;
        }

        auto frozen = builder.Freeze();
        if (!frozen.ok()) {
            return frozen.status();
        }

        context.out << "scan completed: "
                    << frozen.value().manifest_id
                    << " assets=" << frozen.value().asset_count
                    << " digest=" << frozen.value().manifest_digest.ToHex()
                    << "\n";
        return Status::Ok();
    
    }
};

class PlanService final {
public:
    static Status Execute(
        const StatusOr<WorkspaceLayout>& layout,
        const std::string& input_path,
        const std::string& target_path,
        CommandContext& context)
    {
        if (target_path.empty()) {
            return Status(
                StatusCode::kInvalidArgument,
                "plan stage requires a target root");
        }

        auto connection = SqliteConnection::Open(layout.value().database);
        if (!connection.ok()) {
            return connection.status();
        }
        const Status schema_status = EnsureSchema(connection.value());
        if (!schema_status.ok()) {
            return schema_status;
        }

        auto frozen_manifest = SqliteManifestBuilder::Reopen(
            connection.value(),
            input_path);
        if (!frozen_manifest.ok()) {
            return frozen_manifest.status();
        }
        auto manifest = frozen_manifest.value().frozen_manifest();
        if (!manifest.ok()) {
            return manifest.status();
        }

        auto physical_assets = ReadManifestAssets(
            connection.value(),
            manifest.value().manifest_id);
        if (!physical_assets.ok()) {
            return physical_assets.status();
        }

        std::vector<LogicalAsset> logical_assets;
        logical_assets.reserve(physical_assets.value().size());
        for (const PhysicalAsset& asset : physical_assets.value()) {
            auto logical = MapPhysicalAssetToLogicalAsset(asset);
            if (!logical.ok()) {
                return logical.status();
            }
            logical_assets.push_back(std::move(logical.value()));
        }

        const TargetCapabilities capabilities =
            LocalDirectoryCapabilitiesV1();
        auto loss_analysis = AnalyzeCapabilityLoss(capabilities);
        if (!loss_analysis.ok()) return loss_analysis.status();
        const MigrationPolicy policy{};
        TargetPathMapper mapper;
        auto mappings = mapper.MapAll(
            logical_assets,
            capabilities,
            policy);
        if (!mappings.ok()) {
            return mappings.status();
        }

        std::vector<MinimalPlanAsset> plan_assets;
        plan_assets.reserve(mappings.value().size());
        for (const PathMapping& mapping : mappings.value()) {
            const auto logical = std::find_if(
                logical_assets.begin(),
                logical_assets.end(),
                [&mapping](const LogicalAsset& candidate) {
                    return candidate.id == mapping.logical_asset_id;
                });
            if (logical == logical_assets.end()
                || !logical->source_path.has_value()
                || logical->members.size() != 1) {
                return Status(
                    StatusCode::kInternal,
                    "logical asset mapping cannot be bound to one source");
            }

            const auto physical = std::find_if(
                physical_assets.value().begin(),
                physical_assets.value().end(),
                [&logical](const PhysicalAsset& candidate) {
                    return PhysicalAssetIdFor(candidate)
                        == logical->members.front();
                });
            if (physical == physical_assets.value().end()) {
                return Status(
                    StatusCode::kInternal,
                    "logical asset member is missing from manifest");
            }

            plan_assets.push_back(MinimalPlanAsset{
                logical->id,
                logical->members.front(),
                logical->source_path.value(),
                mapping.target_path,
                physical->identity,
            });
        }

        auto plan = CanonicalMinimalPlan::Build(PlannerInput{
            manifest.value().manifest_id,
            manifest.value().manifest_digest,
            target_path,
            capabilities,
            policy,
            std::move(plan_assets),
        });
        if (!plan.ok()) {
            return plan.status();
        }

        const std::string plan_id = "plan-"
            + SemanticDigestFor(plan.value()).ToHex();
        auto artifact = WriteFrozenPlan(plan.value(), plan_id);
        if (!artifact.ok()) {
            return artifact.status();
        }

        const std::filesystem::path artifact_path = layout.value().plans
            / (plan_id + ".plan.jsonl");
        const Status write_status = WritePlanFile(
            artifact_path,
            artifact.value().bytes);
        if (!write_status.ok()) {
            return write_status;
        }

        context.out << "plan completed: " << artifact_path.string()
                    << " assets=" << artifact.value().plan.assets().size()
                    << " artifact_digest="
                    << artifact.value().artifact_digest.ToHex()
                    << " semantic_digest="
                    << artifact.value().semantic_digest.ToHex()
                    << " capability_losses="
                    << loss_analysis.value().losses.size()
                    << "\n";
        return Status::Ok();
    
    }
};

class MigrationService final {
public:
    static Status Execute(
        const StatusOr<WorkspaceLayout>& layout,
        const std::string& input_path,
        CommandContext& context)
    {
        auto plan_bytes = ReadPlanFile(std::filesystem::path(input_path));
        if (!plan_bytes.ok()) {
            return plan_bytes.status();
        }
        auto artifact = ReadFrozenPlan(plan_bytes.value());
        if (!artifact.ok()) {
            return artifact.status();
        }

        if (artifact.value().plan.assets().empty()) {
            return Status(
                StatusCode::kInvalidArgument,
                "migration plan must contain at least one asset");
        }
        auto plan_lock = PlanExecutionLock::Acquire(
            layout.value().locks / (artifact.value().plan_id + ".lock"));
        if (!plan_lock.ok()) return plan_lock.status();
        auto connection = SqliteConnection::Open(layout.value().database);
        if (!connection.ok()) {
            return connection.status();
        }
        const Status schema_status = EnsureSchema(connection.value());
        if (!schema_status.ok()) {
            return schema_status;
        }

        std::size_t task_count = 0;
        const Status materialize_status = MaterializePlan(
            connection.value(),
            artifact.value(),
            std::filesystem::path(input_path),
            task_count);
        if (!materialize_status.ok()) {
            return materialize_status;
        }

        TaskRuntimeRepository repository(connection.value());
        auto state_counts = ReadTaskStateCounts(
            connection.value(), artifact.value().plan_id);
        if (!state_counts.ok()) return state_counts.status();
        if (state_counts.value()[static_cast<std::size_t>(TaskState::kRunning)]
            != 0) {
            return Status(
                StatusCode::kInvalidArgument,
                "migrate found RUNNING task; resume is required before migrate");
        }
        bool ready_found = false;
        std::string ready_task_id;
        std::uint32_t next_attempt_count = 1;
        for (std::size_t index = 0;
             index < artifact.value().plan.assets().size();
             ++index) {
            auto candidate = TaskSpecForPlanAsset(
                artifact.value().plan.assets()[index]);
            if (!candidate.ok()) {
                return candidate.status();
            }
            auto runtime = repository.ReadRuntime(
                artifact.value().plan_id,
                candidate.value().id);
            if (!runtime.ok()) {
                return runtime.status();
            }
            if (runtime.value().state == TaskState::kReady
                || runtime.value().state == TaskState::kRetryable) {
                if (runtime.value().state == TaskState::kRetryable) {
                    const Status ready_status = repository.SetReady(
                        artifact.value().plan_id,
                        candidate.value().id);
                    if (!ready_status.ok()) return ready_status;
                }
                if (!ready_found || candidate.value().id < ready_task_id) {
                    ready_found = true;
                    ready_task_id = candidate.value().id;
                    next_attempt_count = runtime.value().attempt_count + 1;
                }
            }
        }
        if (!ready_found) {
            return Status(
                StatusCode::kNotFound,
                "no READY task remains in the materialized plan");
        }

        auto acquired_epoch = repository.AcquireNextExecutionEpoch(
            artifact.value().plan_id);
        if (!acquired_epoch.ok()) return acquired_epoch.status();
        const ExecutionEpoch execution_epoch = acquired_epoch.value();

        auto source_root = ReadManifestSourceRoot(
            connection.value(),
            artifact.value().plan.source_manifest_id());
        if (!source_root.ok()) {
            return source_root.status();
        }

        const std::string attempt_id =
            "attempt-" + artifact.value().plan_id + "-"
            + ready_task_id + "-"
            + std::to_string(next_attempt_count);
        auto claimed = repository.ClaimNextReady(
            artifact.value().plan_id,
            execution_epoch,
            attempt_id);
        if (!claimed.ok()) {
            return claimed.status();
        }

        std::size_t claimed_index = artifact.value().plan.assets().size();
        for (std::size_t index = 0;
             index < artifact.value().plan.assets().size();
             ++index) {
            auto candidate = TaskSpecForPlanAsset(
                artifact.value().plan.assets()[index]);
            if (!candidate.ok()) return candidate.status();
            if (candidate.value().id == claimed.value().id) {
                claimed_index = index;
                break;
            }
        }
        if (claimed_index == artifact.value().plan.assets().size()) {
            return Status(
                StatusCode::kInternal,
                "claimed task is missing from frozen plan");
        }
        const MinimalPlanAsset& plan_asset =
            artifact.value().plan.assets()[claimed_index];

        LinuxFileOps file_ops;
        auto source_root_fd = file_ops.OpenRoot(
            std::filesystem::path(source_root.value()),
            OpenRootMode::kExisting);
        if (!source_root_fd.ok()) {
            const Status error = source_root_fd.status();
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                execution_epoch,
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }

        const PhysicalAsset source_asset{
            plan_asset.source_path,
            plan_asset.source_identity,
            AssetKind::kUnknown,
            {},
        };
        auto source_guard = MutationGuard::Open(
            file_ops,
            source_root_fd.value().get(),
            source_asset);
        if (!source_guard.ok()) {
            const Status error = source_guard.status();
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                execution_epoch,
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }

        auto target_root_fd = file_ops.OpenRoot(
            std::filesystem::path(artifact.value().plan.target_root()),
            OpenRootMode::kCreateIfMissing);
        if (!target_root_fd.ok()) {
            const Status error = target_root_fd.status();
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                execution_epoch,
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }

        const std::string temp_name = TempNameFor(
            artifact.value().plan_id,
            claimed.value().id,
            attempt_id);
        std::array<std::byte, 1024 * 1024> buffer{};
        auto receipt = MigrationAttemptPreparer::Prepare(
            file_ops,
            repository,
            source_guard.value(),
            target_root_fd.value().get(),
            plan_asset,
            artifact.value().plan_id,
            claimed.value().id,
            execution_epoch,
            attempt_id,
            temp_name,
            std::span<std::byte>(buffer));
        if (!receipt.ok()) {
            const Status error = receipt.status();
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                execution_epoch,
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }

        context.out << "migrate claimed: " << artifact.value().plan_id
            << " task=" << claimed.value().id
                    << " tasks=" << task_count
                    << " source_bytes="
                    << source_guard.value().manifest_identity().size
                    << " temp=" << temp_name
                    << " digest=" << receipt.value().source_digest->ToHex()
                    << " receipt=VERIFIED_DURABLE"
                    << " state=RUNNING\n";
        return Status::Ok();
    
    }
};

class RecoveryService final {
public:
    static Status Execute(
        const StatusOr<WorkspaceLayout>& layout,
        const std::string& input_path,
        CommandContext& context)
    {
        auto plan_bytes = ReadPlanFile(std::filesystem::path(input_path));
        if (!plan_bytes.ok()) {
            return plan_bytes.status();
        }
        auto artifact = ReadFrozenPlan(plan_bytes.value());
        if (!artifact.ok()) {
            return artifact.status();
        }
        if (artifact.value().plan.assets().empty()) {
            return Status(
                StatusCode::kInvalidArgument,
                "resume plan must contain at least one asset");
        }
        auto plan_lock = PlanExecutionLock::Acquire(
            layout.value().locks / (artifact.value().plan_id + ".lock"));
        if (!plan_lock.ok()) return plan_lock.status();
        auto connection = SqliteConnection::Open(layout.value().database);
        if (!connection.ok()) {
            return connection.status();
        }
        const Status schema_status = EnsureSchema(connection.value());
        if (!schema_status.ok()) {
            return schema_status;
        }

        TaskRuntimeRepository repository(connection.value());
        std::size_t selected_index = artifact.value().plan.assets().size();
        std::size_t running_count = 0;
        for (std::size_t index = 0;
             index < artifact.value().plan.assets().size();
             ++index) {
            auto candidate = TaskSpecForPlanAsset(
                artifact.value().plan.assets()[index]);
            if (!candidate.ok()) return candidate.status();
            auto candidate_runtime = repository.ReadRuntime(
                artifact.value().plan_id,
                candidate.value().id);
            if (!candidate_runtime.ok()) return candidate_runtime.status();
            if (candidate_runtime.value().state == TaskState::kRunning) {
                ++running_count;
                selected_index = index;
            }
        }
        if (running_count == 0) {
            return Status(
                StatusCode::kNotFound,
                "no RUNNING task remains in the materialized plan");
        }
        if (running_count != 1) {
            return Status(
                StatusCode::kInvalidArgument,
                "resume requires exactly one RUNNING task in V1");
        }

        auto task = TaskSpecForPlanAsset(
            artifact.value().plan.assets()[selected_index]);
        if (!task.ok()) return task.status();
        auto runtime = repository.ReadRuntime(
            artifact.value().plan_id,
            task.value().id);
        if (!runtime.ok()) {
            return runtime.status();
        }
        if (runtime.value().state != TaskState::kRunning
            || !runtime.value().attempt_id.has_value()
            || runtime.value().owner_epoch.value == 0) {
            return Status(
                StatusCode::kInvalidArgument,
                "resume requires a running task with an active attempt");
        }
        const MinimalPlanAsset& plan_asset =
            artifact.value().plan.assets()[selected_index];
        const RelativePath& target_path = plan_asset.target_path;
        if (target_path.components().size() != 1) {
            return Status(
                StatusCode::kInvalidArgument,
                "single-thread resume requires a single target path component");
        }

        const std::string attempt_id = runtime.value().attempt_id.value();
        CommitIntent intent{
            task.value().id,
            attempt_id,
            runtime.value().owner_epoch,
            TempNameFor(
                artifact.value().plan_id,
                task.value().id,
                attempt_id),
            std::string(target_path.bytes()),
        };
        std::optional<VerifiedReceipt> receipt;
        auto stored_receipt = repository.ReadVerifiedReceipt(
            artifact.value().plan_id,
            task.value().id,
            attempt_id);
        if (stored_receipt.ok()) {
            receipt = stored_receipt.value();
            intent.temp_path = receipt->temp_path;
            const bool receipt_matches_plan =
                receipt->task_id == task.value().id
                && receipt->attempt_id == attempt_id
                && receipt->owner_epoch == runtime.value().owner_epoch
                && receipt->final_path == target_path.bytes()
                && receipt->content_size == plan_asset.source_identity.size
                && receipt->source_identity.has_value()
                && receipt->source_identity.value() == plan_asset.source_identity
                && receipt->source_digest.has_value()
                && receipt->source_digest.value() == receipt->target_digest;
            if (!receipt_matches_plan) {
                auto recovery_epoch = repository.AcquireNextExecutionEpoch(
                    artifact.value().plan_id);
                if (!recovery_epoch.ok()) return recovery_epoch.status();
                const Status recovery_status = repository.RecoverInconsistent(
                    artifact.value().plan_id,
                    task.value().id,
                    recovery_epoch.value(),
                    runtime.value().owner_epoch,
                    attempt_id,
                    "verified receipt does not match the frozen plan");
                if (!recovery_status.ok()) return recovery_status;
                return Status(
                    StatusCode::kInternal,
                    "verified receipt does not match the frozen plan");
            }
        } else if (stored_receipt.status().code() != StatusCode::kNotFound) {
            return stored_receipt.status();
        }

        auto source_root = ReadManifestSourceRoot(
            connection.value(),
            artifact.value().plan.source_manifest_id());
        if (!source_root.ok()) return source_root.status();

        LinuxFileOps file_ops;
        auto source_root_fd = file_ops.OpenRoot(
            std::filesystem::path(source_root.value()),
            OpenRootMode::kExisting);
        if (!source_root_fd.ok()) return source_root_fd.status();
        auto target_root_fd = file_ops.OpenRoot(
            std::filesystem::path(artifact.value().plan.target_root()),
            OpenRootMode::kExisting);
        if (!target_root_fd.ok()) return target_root_fd.status();

        auto recovery_epoch = repository.AcquireNextExecutionEpoch(
            artifact.value().plan_id);
        if (!recovery_epoch.ok()) return recovery_epoch.status();

        ObservedFileState observed;
        auto source_path_fd = file_ops.OpenSource(
            source_root_fd.value().get(), plan_asset.source_path);
        if (source_path_fd.ok()) {
            observed.source_available = true;
            auto source_identity = file_ops.StatFd(source_path_fd.value().get());
            if (!source_identity.ok()) return source_identity.status();
            observed.source_identity = source_identity.value();
        } else if (source_path_fd.status().code() != StatusCode::kNotFound) {
            return source_path_fd.status();
        }

        const std::uint64_t expected_size = receipt.has_value()
            ? receipt->content_size
            : plan_asset.source_identity.size;
        const Digest expected_digest = receipt.has_value()
            ? receipt->target_digest
            : Digest{};
        std::array<std::byte, 1024 * 1024> buffer{};
        const StatusOr<bool> temp_observed = ObserveRecoveryFile(
            file_ops,
            target_root_fd.value().get(),
            receipt.has_value() ? receipt->temp_path : intent.temp_path,
            expected_size,
            expected_digest,
            true,
            observed,
            std::span<std::byte>(buffer));
        if (!temp_observed.ok()) return temp_observed.status();
        const StatusOr<bool> final_observed = ObserveRecoveryFile(
            file_ops,
            target_root_fd.value().get(),
            intent.final_path,
            expected_size,
            expected_digest,
            false,
            observed,
            std::span<std::byte>(buffer));
        if (!final_observed.ok()) return final_observed.status();

        const auto decision = Decide(
            task.value(),
            runtime.value(),
            intent,
            receipt,
            observed);
        if (!decision.ok()) return decision.status();
        context.out << RenderAuditReport(RecoveryAudit(
            artifact.value().plan_id,
            task.value().id,
            intent,
            receipt,
            observed,
            decision.value()));
        PauseForTest("PHOTOBRIDGE_TEST_PAUSE_BEFORE_RECOVERY_MS");

        const auto recover_retryable = [&repository, &artifact, &task, &runtime,
                                        &attempt_id, &recovery_epoch](
            std::string_view reason) {
            return repository.RecoverRetryable(
                artifact.value().plan_id,
                task.value().id,
                recovery_epoch.value(),
                runtime.value().owner_epoch,
                attempt_id,
                std::string(reason));
        };
        const auto recover_inconsistent = [&repository, &artifact, &task,
                                           &runtime, &attempt_id,
                                           &recovery_epoch](std::string_view reason) {
            return repository.RecoverInconsistent(
                artifact.value().plan_id,
                task.value().id,
                recovery_epoch.value(),
                runtime.value().owner_epoch,
                attempt_id,
                std::string(reason));
        };
        const auto recover_succeeded = [&repository, &artifact, &task, &runtime,
                                        &attempt_id, &recovery_epoch](
            std::string_view reason) {
            return repository.RecoverSucceeded(
                artifact.value().plan_id,
                task.value().id,
                recovery_epoch.value(),
                runtime.value().owner_epoch,
                attempt_id,
                std::string(reason));
        };

        switch (decision.value().action) {
        case ReconcileAction::kRetryTask: {
            const Status status = recover_retryable(decision.value().reason);
            if (!status.ok()) return status;
            return Status(StatusCode::kIoError, decision.value().reason);
        }
        case ReconcileAction::kTargetConflict: {
            const Status status = recover_inconsistent(decision.value().reason);
            if (!status.ok()) return status;
            return Status(StatusCode::kAlreadyExists, decision.value().reason);
        }
        case ReconcileAction::kInconsistent: {
            const Status status = recover_inconsistent(decision.value().reason);
            if (!status.ok()) return status;
            return Status(StatusCode::kInternal, decision.value().reason);
        }
        case ReconcileAction::kAdoptFinal:
        case ReconcileAction::kAdoptFinalAndCleanupTemp: {
            ObservedFileState confirmation;
            const StatusOr<bool> confirmed_final = ObserveRecoveryFile(
                file_ops,
                target_root_fd.value().get(),
                intent.final_path,
                expected_size,
                expected_digest,
                false,
                confirmation,
                std::span<std::byte>(buffer));
            if (!confirmed_final.ok() || !confirmed_final.value()
                || confirmation.final_size != expected_size
                || !confirmation.final_digest.has_value()
                || confirmation.final_digest.value() != expected_digest) {
                const Status status = recover_retryable(
                    "final changed before recovery commit");
                return status.ok()
                    ? Status(StatusCode::kIoError,
                             "final changed before recovery commit")
                    : status;
            }
            if (decision.value().action
                    == ReconcileAction::kAdoptFinalAndCleanupTemp) {
                const StatusOr<bool> confirmed_temp = ObserveRecoveryFile(
                    file_ops,
                    target_root_fd.value().get(),
                    receipt->temp_path,
                    expected_size,
                    expected_digest,
                    true,
                    confirmation,
                    std::span<std::byte>(buffer));
                if (!confirmed_temp.ok() || !confirmed_temp.value()
                    || confirmation.temp_size != expected_size
                    || !confirmation.temp_digest.has_value()
                    || confirmation.temp_digest.value() != expected_digest) {
                    const Status status = recover_retryable(
                        "verified temp changed before cleanup");
                    return status.ok()
                        ? Status(StatusCode::kIoError,
                                 "verified temp changed before cleanup")
                        : status;
                }
                const Status unlink_status = file_ops.UnlinkAt(
                    target_root_fd.value().get(), receipt->temp_path);
                if (!unlink_status.ok()) return unlink_status;
                const Status sync_status = file_ops.FsyncDirectory(
                    target_root_fd.value().get());
                if (!sync_status.ok()) return sync_status;
            }
            const Status success_status = recover_succeeded(decision.value().reason);
            if (!success_status.ok()) return success_status;
            context.out << "resume completed: " << artifact.value().plan_id
                        << " task=" << task.value().id
                        << " target=" << target_path.bytes()
                        << " state=SUCCEEDED\n";
            return Status::Ok();
        }
        case ReconcileAction::kResumeCommitFromTemp: {
            auto temp_path = RelativePath::Parse(intent.temp_path);
            if (!temp_path.ok()) return temp_path.status();
            auto temp_fd = file_ops.OpenSource(
                target_root_fd.value().get(), temp_path.value());
            if (!temp_fd.ok()) return temp_fd.status();
            Blake3Hasher hasher;
            auto verification = VerifyBinary(
                file_ops,
                hasher,
                temp_fd.value().get(),
                expected_size,
                std::optional<Digest>(expected_digest),
                std::span<std::byte>(buffer));
            if (!verification.ok()) return verification.status();
            if (verification.value().status != BinaryVerification::kIdentical) {
                const Status status = recover_retryable(
                    "verified temp no longer matches receipt");
                return status.ok()
                    ? Status(StatusCode::kIoError,
                             "verified temp no longer matches receipt")
                    : status;
            }
            Status sync_status = file_ops.Fdatasync(temp_fd.value().get());
            if (!sync_status.ok()) return sync_status;
            sync_status = file_ops.RenameNoReplace(
                target_root_fd.value().get(),
                intent.temp_path,
                target_root_fd.value().get(),
                intent.final_path);
            if (!sync_status.ok()) return sync_status;
            PauseForTest("PHOTOBRIDGE_TEST_PAUSE_AFTER_RENAME_MS");
            sync_status = file_ops.FsyncDirectory(target_root_fd.value().get());
            if (!sync_status.ok()) return sync_status;
            const Status success_status = recover_succeeded(decision.value().reason);
            if (!success_status.ok()) return success_status;
            context.out << "resume completed: " << artifact.value().plan_id
                        << " task=" << task.value().id
                        << " target=" << target_path.bytes()
                        << " state=SUCCEEDED\n";
            return Status::Ok();
        }
        case ReconcileAction::kReverifyCommitted:
            return Status(
                StatusCode::kInvalidArgument,
                "resume cannot reverify a non-running task");
        }
        return Status(
            StatusCode::kInternal,
            "recovery service reached an unreachable state");
    }
};

class VerifyService final {
public:
    static Status Execute(
        const StatusOr<WorkspaceLayout>& layout,
        const std::string& input_path,
        CommandContext& context)
    {
        auto plan_bytes = ReadPlanFile(std::filesystem::path(input_path));
        if (!plan_bytes.ok()) {
            return plan_bytes.status();
        }
        auto artifact = ReadFrozenPlan(plan_bytes.value());
        if (!artifact.ok()) {
            return artifact.status();
        }
        if (artifact.value().plan.assets().empty()) {
            return Status(
                StatusCode::kInvalidArgument,
                "verification plan must contain at least one asset");
        }

        auto connection = SqliteConnection::Open(layout.value().database);
        if (!connection.ok()) {
            return connection.status();
        }
        const Status schema_status = EnsureSchema(connection.value());
        if (!schema_status.ok()) {
            return schema_status;
        }

        TaskRuntimeRepository repository(connection.value());
        auto source_root = ReadManifestSourceRoot(
            connection.value(),
            artifact.value().plan.source_manifest_id());
        if (!source_root.ok()) {
            return source_root.status();
        }

        LinuxFileOps file_ops;
        auto source_root_fd = file_ops.OpenRoot(
            std::filesystem::path(source_root.value()),
            OpenRootMode::kExisting);
        if (!source_root_fd.ok()) {
            return source_root_fd.status();
        }

        auto target_root_fd = file_ops.OpenRoot(
            std::filesystem::path(artifact.value().plan.target_root()),
            OpenRootMode::kExisting);
        if (!target_root_fd.ok()) {
            return target_root_fd.status();
        }

        std::array<std::byte, 1024 * 1024> buffer{};
        std::uint64_t verified_bytes = 0;
        std::vector<DiffFileState> expected_states;
        std::vector<DiffFileState> observed_states;
        expected_states.reserve(artifact.value().plan.assets().size());
        observed_states.reserve(artifact.value().plan.assets().size());
        for (const MinimalPlanAsset& plan_asset : artifact.value().plan.assets()) {
            auto task = TaskSpecForPlanAsset(plan_asset);
            if (!task.ok()) return task.status();
            auto runtime = repository.ReadRuntime(
                artifact.value().plan_id,
                task.value().id);
            if (!runtime.ok()) return runtime.status();
            if (runtime.value().state != TaskState::kSucceeded) {
                return Status(
                    StatusCode::kInvalidArgument,
                    "binary verification requires every task to be succeeded");
            }
            auto bytes = VerifyPlanAsset(
                file_ops,
                source_root_fd.value().get(),
                target_root_fd.value().get(),
                plan_asset,
                std::span<std::byte>(buffer));
            if (!bytes.ok()) return bytes.status();
            if (verified_bytes > std::numeric_limits<std::uint64_t>::max()
                    - bytes.value().bytes) {
                return Status(
                    StatusCode::kInternal,
                    "verified byte count overflowed");
            }
            verified_bytes += bytes.value().bytes;
            expected_states.push_back(std::move(bytes.value().expected));
            observed_states.push_back(std::move(bytes.value().observed));
        }
        const std::vector<DiffEntry> diff = DiffFileStates(
            expected_states,
            observed_states);
        context.out << RenderAuditReport(AuditReport{
            artifact.value().plan_id,
            {},
            "verify",
            diff.empty() ? "IDENTICAL" : "MISMATCH",
            diff,
            diff.empty() ? "" : "target differs from source",
        });
        if (!diff.empty()) {
            return Status(
                StatusCode::kInternal,
                "target binary verification did not match source");
        }
        context.out << "verify completed: " << artifact.value().plan_id
                    << " tasks=" << artifact.value().plan.assets().size()
                    << " status=IDENTICAL bytes=" << verified_bytes
                    << "\n";
        return Status::Ok();
    
    }
};

class StatusService final {
public:
    static Status Execute(
        const StatusOr<WorkspaceLayout>& layout,
        const std::string& input_path,
        CommandContext& context)
    {
        auto plan_bytes = ReadPlanFile(std::filesystem::path(input_path));
        if (!plan_bytes.ok()) {
            return plan_bytes.status();
        }
        auto artifact = ReadFrozenPlan(plan_bytes.value());
        if (!artifact.ok()) {
            return artifact.status();
        }

        auto connection = SqliteConnection::Open(layout.value().database);
        if (!connection.ok()) {
            return connection.status();
        }

        PipelineStatement plan_statement(
            connection.value().native_handle(),
            "SELECT artifact_digest, semantic_digest FROM migration_plan "
            "WHERE plan_id = ?1;");
        if (plan_statement.result() != SQLITE_OK) {
            return SqliteReadError(
                connection.value().native_handle(),
                "prepare status plan lookup");
        }
        Status status = BindPipelineText(
            plan_statement.get(),
            1,
            artifact.value().plan_id);
        if (!status.ok()) return status;
        const int plan_step = sqlite3_step(plan_statement.get());
        if (plan_step == SQLITE_DONE) {
            return Status(
                StatusCode::kNotFound,
                "materialized plan was not found: "
                    + artifact.value().plan_id);
        }
        if (plan_step != SQLITE_ROW) {
            return SqliteReadError(
                connection.value().native_handle(),
                "read status plan lookup");
        }
        for (const int column : {0, 1}) {
            if (sqlite3_column_type(plan_statement.get(), column) != SQLITE_BLOB
                || sqlite3_column_bytes(plan_statement.get(), column)
                    != static_cast<int>(artifact.value().artifact_digest.bytes.size())) {
                return Status(
                    StatusCode::kInternal,
                    "status plan digest has an invalid type or size");
            }
        }
        const auto digest_matches = [
            &plan_statement](int column, const Digest& expected) {
            return std::equal(
                expected.bytes.begin(),
                expected.bytes.end(),
                static_cast<const std::byte*>(sqlite3_column_blob(
                    plan_statement.get(),
                    column)));
        };
        if (!digest_matches(0, artifact.value().artifact_digest)
            || !digest_matches(1, artifact.value().semantic_digest)) {
            return Status(
                StatusCode::kInternal,
                "frozen plan digest does not match SQLite binding");
        }

        auto counts = ReadTaskStateCounts(
            connection.value(),
            artifact.value().plan_id);
        if (!counts.ok()) {
            return counts.status();
        }
        std::uint64_t task_count = 0;
        for (const std::uint64_t count : counts.value()) {
            task_count += count;
        }
        context.out << "status: " << artifact.value().plan_id
                    << " tasks=" << task_count;
        for (std::size_t index = 0; index < counts.value().size(); ++index) {
            if (counts.value()[index] == 0) continue;
            context.out << " "
                        << TaskStateName(static_cast<TaskState>(index))
                        << "=" << counts.value()[index];
        }
        context.out << "\n";
        return Status::Ok();
    
    }
};

Status RunPipelineStage(
    PipelineStage stage,
    std::string workspace_path,
    std::string input_path,
    std::string target_path,
    CommandContext& context)
{

    auto layout = WorkspaceLayout::FromRoot(
        std::filesystem::path(workspace_path));
    if (!layout.ok()) {
        return layout.status();
    }

    if (RequiresInput(stage) && input_path.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "pipeline stage requires an input path");
    }


    switch (stage) {
    case PipelineStage::kScan:
        return ScanService::Execute(layout, input_path, context);
    case PipelineStage::kPlan:
        return PlanService::Execute(layout, input_path, target_path, context);
    case PipelineStage::kMigrate:
        return MigrationService::Execute(layout, input_path, context);
    case PipelineStage::kResume:
        return RecoveryService::Execute(layout, input_path, context);
    case PipelineStage::kVerify:
        return VerifyService::Execute(layout, input_path, context);
    case PipelineStage::kStatus:
        return StatusService::Execute(layout, input_path, context);
    }
    return Status(StatusCode::kInvalidArgument, "unknown pipeline stage");
}

}  // namespace photobridge
