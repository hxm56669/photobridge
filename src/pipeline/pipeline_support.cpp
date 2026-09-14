#include "photobridge/pipeline/pipeline_support.h"

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

namespace photobridge::pipeline {

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

}  // namespace photobridge::pipeline
