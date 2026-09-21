#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "photobridge/app/sqlite_connection.h"
#include "photobridge/app/manifest_builder.h"
#include "photobridge/app/migration_attempt_preparer.h"
#include "photobridge/app/plan_execution_lock.h"
#include "photobridge/app/sqlite_schema.h"
#include "photobridge/app/task_runtime_repository.h"
#include "photobridge/app/workspace_layout.h"
#include "photobridge/cli/pipeline_services.h"
#include "photobridge/common/posix_error.h"
#include "photobridge/common/test_hooks.h"
#include "photobridge/common/time.h"
#include "photobridge/filesystem/binary_verifier.h"
#include "photobridge/filesystem/copy_and_hash.h"
#include "photobridge/filesystem/file_ops.h"
#include "photobridge/filesystem/linux_file_ops.h"
#include "photobridge/filesystem/mutation_guard.h"
#include "photobridge/filesystem/reconciler.h"
#include "photobridge/model/audit_report.h"
#include "photobridge/model/canonical_plan.h"
#include "photobridge/model/capability.h"
#include "photobridge/model/diff_engine.h"
#include "photobridge/model/loss_analysis.h"
#include "photobridge/model/logical_asset.h"
#include "photobridge/model/path_mapper.h"
#include "photobridge/model/physical_asset.h"
#include "photobridge/model/plan_artifact.h"
#include "photobridge/model/task.h"
#include "photobridge/source/local_folder_source.h"

namespace photobridge::pipeline {

Status SqliteReadError(
    sqlite3* database,
    std::string_view operation);

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

StatusOr<std::string> ReadPlanFile(const std::filesystem::path& path);
StatusOr<std::array<std::uint64_t, 9>> ReadTaskStateCounts(
    SqliteConnection& connection,
    std::string_view plan_id);
StatusOr<std::string> ReadManifestSourceRoot(
    SqliteConnection& connection,
    std::string_view manifest_id);
Status BindPipelineText(
    sqlite3_stmt* statement,
    int index,
    std::string_view value);
Status BindPipelineBlob(
    sqlite3_stmt* statement,
    int index,
    const void* data,
    std::size_t size);
Status RollbackMaterialization(SqliteConnection& connection, Status status);
StatusOr<TaskSpec> TaskSpecForPlanAsset(const MinimalPlanAsset& asset);
std::string TempNameFor(
    std::string_view plan_id,
    std::string_view task_id,
    std::string_view attempt_id);
StatusOr<bool> ObserveRecoveryFile(
    LinuxFileOps& file_ops,
    int root_fd,
    const std::string& path_bytes,
    std::uint64_t expected_size,
    const Digest& expected_digest,
    bool temp,
    ObservedFileState& observed,
    std::span<std::byte> buffer);
StatusOr<bool> CheckMaterializedPlan(
    SqliteConnection& connection,
    const FrozenPlanFile& artifact);
Status MaterializePlan(
    SqliteConnection& connection,
    const FrozenPlanFile& artifact,
    const std::filesystem::path& artifact_path,
    std::size_t& task_count);

Status RunScanService(
    const StatusOr<WorkspaceLayout>& layout,
    const std::string& input_path,
    CommandContext& context);
Status RunPlanService(
    const StatusOr<WorkspaceLayout>& layout,
    const std::string& input_path,
    const std::string& target_path,
    CommandContext& context);
Status RunMigrationService(
    const StatusOr<WorkspaceLayout>& layout,
    const std::string& input_path,
    std::size_t workers,
    std::size_t db_batch_size,
    CommandContext& context);
Status RunRecoveryService(
    const StatusOr<WorkspaceLayout>& layout,
    const std::string& input_path,
    CommandContext& context);
Status RunVerifyService(
    const StatusOr<WorkspaceLayout>& layout,
    const std::string& input_path,
    CommandContext& context);
Status RunStatusService(
    const StatusOr<WorkspaceLayout>& layout,
    const std::string& input_path,
    CommandContext& context);

}  // namespace photobridge::pipeline
