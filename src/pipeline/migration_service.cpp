#include "photobridge/pipeline/pipeline_support.h"

namespace photobridge::pipeline {

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


Status RunMigrationService(
    const StatusOr<WorkspaceLayout>& layout,
    const std::string& input_path,
    CommandContext& context)
{
    return MigrationService::Execute(layout, input_path, context);
}

}  // namespace photobridge::pipeline
