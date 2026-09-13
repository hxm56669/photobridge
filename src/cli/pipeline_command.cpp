#include "photobridge/cli/pipeline_command.h"

#include <sqlite3.h>
#include <blake3.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cstddef>
#include <span>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include "photobridge/app/manifest_builder.h"
#include "photobridge/app/sqlite_connection.h"
#include "photobridge/app/sqlite_schema.h"
#include "photobridge/app/task_runtime_repository.h"
#include "photobridge/app/workspace_layout.h"
#include "photobridge/common/posix_error.h"
#include "photobridge/filesystem/binary_verifier.h"
#include "photobridge/filesystem/copy_and_hash.h"
#include "photobridge/filesystem/linux_file_ops.h"
#include "photobridge/model/canonical_plan.h"
#include "photobridge/model/capability.h"
#include "photobridge/model/loss_analysis.h"
#include "photobridge/model/logical_asset.h"
#include "photobridge/model/path_mapper.h"
#include "photobridge/model/plan_artifact.h"
#include "photobridge/source/local_folder_source.h"

namespace photobridge {
namespace {

std::string_view StageName(PipelineStage stage)
{
    switch (stage) {
    case PipelineStage::kScan:
        return "scan";
    case PipelineStage::kPlan:
        return "plan";
    case PipelineStage::kMigrate:
        return "migrate";
    case PipelineStage::kResume:
        return "resume";
    case PipelineStage::kVerify:
        return "verify";
    case PipelineStage::kStatus:
        return "status";
    }

    return "unknown";
}

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

StatusOr<UniqueFd> AcquirePlanLock(const std::filesystem::path& lock_path)
{
    const int fd = ::open(
        lock_path.c_str(),
        O_CREAT | O_RDWR | O_CLOEXEC,
        0600);
    if (fd < 0) {
        return StatusFromErrno(errno, "open plan lock");
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int error_number = errno;
        ::close(fd);
        if (error_number == EWOULDBLOCK || error_number == EAGAIN) {
            return Status(
                StatusCode::kAlreadyExists,
                "plan is already being resumed: " + lock_path.string());
        }
        return StatusFromErrno(error_number, "acquire plan lock");
    }
    return UniqueFd(fd);
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

std::int64_t CurrentTimeNanoseconds()
{
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return now.count();
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

StatusOr<std::uint64_t> VerifyPlanAsset(
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

    auto source_fd = file_ops.OpenSource(source_root_fd, plan_asset.source_path);
    if (!source_fd.ok()) return source_fd.status();
    auto source_identity = file_ops.StatFd(source_fd.value().get());
    if (!source_identity.ok()) return source_identity.status();
    if (source_identity.value() != plan_asset.source_identity) {
        return Status(
            StatusCode::kInternal,
            "source file identity does not match frozen plan");
    }

    Blake3Hasher source_hasher;
    auto source_verification = VerifyBinary(
        file_ops,
        source_hasher,
        source_fd.value().get(),
        plan_asset.source_identity.size,
        std::nullopt,
        buffer);
    if (!source_verification.ok()) return source_verification.status();
    if (source_verification.value().target_before
            != plan_asset.source_identity
        || source_verification.value().target_after
            != plan_asset.source_identity
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
    if (target_verification.value().status != BinaryVerification::kIdentical) {
        return Status(
            StatusCode::kInternal,
            "target binary verification did not match source");
    }
    return target_verification.value().bytes_read;
}

}  // namespace

PipelineCommand::PipelineCommand(
    PipelineStage stage,
    std::string workspace_path,
    std::string input_path,
    std::string target_path)
    : stage_(stage),
      workspace_path_(std::move(workspace_path)),
      input_path_(std::move(input_path)),
      target_path_(std::move(target_path))
{
}

Status PipelineCommand::Execute(CommandContext& context)
{
    auto layout = WorkspaceLayout::FromRoot(
        std::filesystem::path(workspace_path_));
    if (!layout.ok()) {
        return layout.status();
    }

    if (RequiresInput(stage_) && input_path_.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "pipeline stage requires an input path");
    }

    if (stage_ == PipelineStage::kScan) {
        auto connection = SqliteConnection::Open(layout.value().database);
        if (!connection.ok()) {
            return connection.status();
        }

        const Status schema_status = EnsureSchema(connection.value());
        if (!schema_status.ok()) {
            return schema_status;
        }

        LocalFolderSource source{std::filesystem::path(input_path_)};
        SqliteManifestBuilder builder(connection.value());
        const Status begin_status = builder.Begin({
            input_path_,
            std::string(source.TypeName()),
            input_path_,
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

    if (stage_ == PipelineStage::kPlan) {
        if (target_path_.empty()) {
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
            input_path_);
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
            target_path_,
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

    if (stage_ == PipelineStage::kMigrate) {
        auto plan_bytes = ReadPlanFile(std::filesystem::path(input_path_));
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
            std::filesystem::path(input_path_),
            task_count);
        if (!materialize_status.ok()) {
            return materialize_status;
        }

        TaskRuntimeRepository repository(connection.value());
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
            ExecutionEpoch{1},
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
                ExecutionEpoch{1},
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }

        auto source_fd = file_ops.OpenSource(
            source_root_fd.value().get(),
            plan_asset.source_path);
        if (!source_fd.ok()) {
            const Status error = source_fd.status();
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                ExecutionEpoch{1},
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }

        auto source_identity = file_ops.StatFd(source_fd.value().get());
        if (!source_identity.ok()) {
            const Status error = source_identity.status();
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                ExecutionEpoch{1},
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }
        if (source_identity.value() != plan_asset.source_identity) {
            const Status error(
                StatusCode::kInternal,
                "source file identity does not match frozen plan");
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                ExecutionEpoch{1},
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }

        if (plan_asset.target_path.components().size() != 1) {
            const Status error(
                StatusCode::kInvalidArgument,
                "single-thread temp preparation requires a single target path component");
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                ExecutionEpoch{1},
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
                ExecutionEpoch{1},
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }

        const std::string temp_name = TempNameFor(
            artifact.value().plan_id,
            claimed.value().id,
            attempt_id);
        auto temp_fd = file_ops.CreateTempNoReplace(
            target_root_fd.value().get(),
            temp_name,
            0600);
        if (!temp_fd.ok()) {
            const Status error = temp_fd.status();
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                ExecutionEpoch{1},
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }

        Blake3Hasher hasher;
        std::array<std::byte, 1024 * 1024> buffer{};
        auto copy = CopyAndHash(
            file_ops,
            hasher,
            source_fd.value().get(),
            temp_fd.value().get(),
            std::span<std::byte>(buffer));
        if (!copy.ok()) {
            const Status error = copy.status();
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                ExecutionEpoch{1},
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }

        const Status sync_status = file_ops.Fdatasync(temp_fd.value().get());
        if (!sync_status.ok()) {
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                ExecutionEpoch{1},
                attempt_id,
                sync_status);
            return retry_status.ok() ? sync_status : retry_status;
        }

        auto temp_path = RelativePath::Parse(temp_name);
        if (!temp_path.ok()) {
            const Status error = temp_path.status();
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                ExecutionEpoch{1},
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }
        auto verify_fd = file_ops.OpenSource(
            target_root_fd.value().get(), temp_path.value());
        if (!verify_fd.ok()) {
            const Status error = verify_fd.status();
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                ExecutionEpoch{1},
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }
        Blake3Hasher target_hasher;
        auto target_verification = VerifyBinary(
            file_ops,
            target_hasher,
            verify_fd.value().get(),
            copy.value().bytes_copied,
            std::optional<Digest>(copy.value().source_digest),
            std::span<std::byte>(buffer));
        if (!target_verification.ok()) {
            const Status error = target_verification.status();
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                ExecutionEpoch{1},
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }
        if (target_verification.value().status != BinaryVerification::kIdentical) {
            const Status error(
                StatusCode::kInternal,
                "temporary file failed independent binary verification");
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                ExecutionEpoch{1},
                attempt_id,
                error);
            return retry_status.ok() ? error : retry_status;
        }

        const VerifiedReceipt receipt{
            claimed.value().id,
            attempt_id,
            ExecutionEpoch{1},
            temp_name,
            std::string(plan_asset.target_path.bytes()),
            copy.value().bytes_copied,
            copy.value().source_digest,
            target_verification.value().target_digest,
            copy.value().source_before,
        };
        const Status receipt_status = repository.PersistVerifiedReceipt(
            artifact.value().plan_id,
            receipt);
        if (!receipt_status.ok()) {
            const Status retry_status = repository.MarkRetryable(
                artifact.value().plan_id,
                claimed.value().id,
                ExecutionEpoch{1},
                attempt_id,
                receipt_status);
            return retry_status.ok() ? receipt_status : retry_status;
        }

        context.out << "migrate claimed: " << artifact.value().plan_id
                    << " task=" << claimed.value().id
                    << " tasks=" << task_count
                    << " source_bytes=" << source_identity.value().size
                    << " temp=" << temp_name
                    << " digest=" << copy.value().source_digest.ToHex()
                    << " receipt=VERIFIED_DURABLE"
                    << " state=RUNNING\n";
        return Status::Ok();
    }

    if (stage_ == PipelineStage::kResume) {
        auto plan_bytes = ReadPlanFile(std::filesystem::path(input_path_));
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
        auto plan_lock = AcquirePlanLock(
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
                selected_index = index;
                break;
            }
        }
        if (selected_index == artifact.value().plan.assets().size()) {
            return Status(
                StatusCode::kNotFound,
                "no RUNNING task remains in the materialized plan");
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
        if (runtime.value().owner_epoch.value != 1) {
            return Status(
                StatusCode::kInvalidArgument,
                "single-thread resume only supports epoch 1");
        }

        const MinimalPlanAsset& plan_asset =
            artifact.value().plan.assets()[selected_index];
        const RelativePath& target_path = plan_asset.target_path;
        if (target_path.components().size() != 1) {
            return Status(
                StatusCode::kInvalidArgument,
                "single-thread resume requires a single target path component");
        }

        LinuxFileOps file_ops;
        auto target_root_fd = file_ops.OpenRoot(
            std::filesystem::path(artifact.value().plan.target_root()),
            OpenRootMode::kExisting);
        if (!target_root_fd.ok()) {
            return target_root_fd.status();
        }

        const std::string temp_name = TempNameFor(
            artifact.value().plan_id,
            task.value().id,
            runtime.value().attempt_id.value());
        const Status rename_status = file_ops.RenameNoReplace(
            target_root_fd.value().get(),
            temp_name,
            target_root_fd.value().get(),
            target_path.bytes());
        if (!rename_status.ok()) {
            return rename_status;
        }

        const Status sync_status = file_ops.FsyncDirectory(
            target_root_fd.value().get());
        if (!sync_status.ok()) {
            return sync_status;
        }

        const Status success_status = repository.MarkSucceeded(
            artifact.value().plan_id,
            task.value().id,
            runtime.value().owner_epoch,
            runtime.value().attempt_id.value());
        if (!success_status.ok()) {
            return success_status;
        }

        context.out << "resume completed: " << artifact.value().plan_id
                    << " task=" << task.value().id
                    << " target=" << target_path.bytes()
                    << " state=SUCCEEDED\n";
        return Status::Ok();
    }

    if (stage_ == PipelineStage::kVerify) {
        auto plan_bytes = ReadPlanFile(std::filesystem::path(input_path_));
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
                    - bytes.value()) {
                return Status(
                    StatusCode::kInternal,
                    "verified byte count overflowed");
            }
            verified_bytes += bytes.value();
        }
        context.out << "verify completed: " << artifact.value().plan_id
                    << " tasks=" << artifact.value().plan.assets().size()
                    << " status=IDENTICAL bytes=" << verified_bytes
                    << "\n";
        return Status::Ok();
    }

    if (stage_ == PipelineStage::kStatus) {
        auto plan_bytes = ReadPlanFile(std::filesystem::path(input_path_));
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

    context.out << StageName(stage_) << " stage accepted: "
                << layout.value().root.string();
    if (!input_path_.empty()) {
        context.out << " <- " << input_path_;
    }
    context.out << "\n";
    return Status::Ok();
}

}  // namespace photobridge
