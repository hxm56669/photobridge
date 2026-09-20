#include "photobridge/pipeline/pipeline_support.h"

namespace photobridge::pipeline {

namespace {

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

}  // namespace

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
        std::vector<std::size_t> running_indices;
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
                running_indices.push_back(index);
            }
        }
        if (running_indices.empty()) {
            return Status(
                StatusCode::kNotFound,
                "no RUNNING task remains in the materialized plan");
        }
        auto recovery_epoch = repository.AcquireNextExecutionEpoch(
            artifact.value().plan_id);
        if (!recovery_epoch.ok()) return recovery_epoch.status();

        const auto recover_one = [&](std::size_t selected_index) -> Status {
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
                "resume requires a single target path component");
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

        ObservedFileState observed;
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
        if (source_guard.ok()) {
            observed.source_available = true;
            observed.source_identity = source_guard.value().manifest_identity();
        } else if (source_guard.status().code() == StatusCode::kNotFound) {
            observed.source_available = false;
        } else if (source_guard.status().code() == StatusCode::kInternal) {
            observed.source_changed = true;
        } else {
            return source_guard.status();
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
        };

        Status first_error = Status::Ok();
        for (const std::size_t index : running_indices) {
            const Status result = recover_one(index);
            if (!result.ok() && first_error.ok()) first_error = result;
        }
        return first_error;
    }
};


Status RunRecoveryService(
    const StatusOr<WorkspaceLayout>& layout,
    const std::string& input_path,
    CommandContext& context)
{
    return RecoveryService::Execute(layout, input_path, context);
}

}  // namespace photobridge::pipeline
