#include "photobridge/filesystem/reconciler.h"

#include <string_view>

namespace photobridge {
namespace {

Status Invalid(const char* message)
{
    return Status(StatusCode::kInvalidArgument, message);
}

Status ValidateIntent(
    const TaskSpec& spec,
    const TaskRuntime& runtime,
    const CommitIntent& intent)
{
    if (spec.type != TaskType::kMigrateFile) {
        return Invalid("reconciler only accepts file migration tasks");
    }
    Status status = ValidateTaskSpec(spec);
    if (!status.ok()) return status;
    status = ValidateTaskRuntime(runtime);
    if (!status.ok()) return status;
    if (intent.task_id.empty() || intent.attempt_id.empty()
        || intent.temp_path.empty() || intent.final_path.empty()) {
        return Invalid("commit intent fields must not be empty");
    }
    if (intent.task_id != spec.id || intent.task_id != runtime.id) {
        return Invalid("commit intent task does not match task runtime");
    }
    if (runtime.attempt_id.has_value()
        && runtime.attempt_id.value() != intent.attempt_id) {
        return Invalid("commit intent attempt does not match task runtime");
    }
    if (runtime.owner_epoch.value != 0
        && runtime.owner_epoch != intent.owner_epoch) {
        return Invalid("commit intent epoch does not match task runtime");
    }
    if (intent.final_path != spec.target_path.bytes()) {
        return Invalid("commit intent final path is not frozen target path");
    }
    return Status::Ok();
}

Status ValidateReceipt(
    const CommitIntent& intent,
    const std::optional<VerifiedReceipt>& receipt)
{
    if (!receipt.has_value()) return Status::Ok();
    const VerifiedReceipt& value = receipt.value();
    if (value.task_id != intent.task_id
        || value.attempt_id != intent.attempt_id
        || value.owner_epoch != intent.owner_epoch
        || value.temp_path != intent.temp_path
        || value.final_path != intent.final_path) {
        return Invalid("verified receipt does not match commit intent");
    }
    return Status::Ok();
}

bool Matches(
    bool exists,
    std::uint64_t size,
    const std::optional<Digest>& digest,
    const VerifiedReceipt& receipt)
{
    return exists && size == receipt.content_size && digest.has_value()
        && digest.value() == receipt.target_digest;
}

ReconcileDecision Decision(ReconcileAction action, std::string_view reason)
{
    return ReconcileDecision{action, std::string(reason)};
}

}  // namespace

StatusOr<ReconcileDecision> Decide(
    const TaskSpec& spec,
    const TaskRuntime& runtime,
    const CommitIntent& intent,
    const std::optional<VerifiedReceipt>& receipt,
    const ObservedFileState& observed)
{
    Status status = ValidateIntent(spec, runtime, intent);
    if (!status.ok()) return status;
    status = ValidateReceipt(intent, receipt);
    if (!status.ok()) return status;

    if (observed.source_changed) {
        return Decision(
            ReconcileAction::kInconsistent,
            "SOURCE_CHANGED: source identity differs from the frozen plan");
    }

    if (!receipt.has_value()) {
        if (observed.final_exists) {
            return Decision(
                ReconcileAction::kTargetConflict,
                "final exists without a durable verified receipt");
        }
        return Decision(
            ReconcileAction::kRetryTask,
            observed.temp_exists
                ? "intent has no receipt; retry without cleaning unproven temp"
                : "intent has no receipt; create a new attempt");
    }

    const VerifiedReceipt& verified = receipt.value();
    const bool temp_matches = Matches(
        observed.temp_exists,
        observed.temp_size,
        observed.temp_digest,
        verified);
    const bool final_matches = Matches(
        observed.final_exists,
        observed.final_size,
        observed.final_digest,
        verified);

    if (observed.final_exists && !final_matches) {
        return Decision(
            ReconcileAction::kTargetConflict,
            "final exists but does not match the verified receipt");
    }
    if (runtime.state == TaskState::kSucceeded) {
        return final_matches
            ? Decision(
                  ReconcileAction::kReverifyCommitted,
                  "committed task final matches receipt")
            : Decision(
                  ReconcileAction::kInconsistent,
                  "committed task final is missing or unverifiable");
    }
    if (final_matches && temp_matches) {
        return Decision(
            ReconcileAction::kAdoptFinalAndCleanupTemp,
            "matching final and temp are durable evidence; clean owned temp after commit");
    }
    if (final_matches) {
        return Decision(
            ReconcileAction::kAdoptFinal,
            "matching final can be adopted from durable receipt");
    }
    if (temp_matches && !observed.final_exists) {
        return Decision(
            ReconcileAction::kResumeCommitFromTemp,
            "matching temp can be re-synced and published under a new epoch");
    }
    if (!observed.temp_exists && !observed.final_exists) {
        return observed.source_available
            ? Decision(
                  ReconcileAction::kRetryTask,
                  "receipt exists but files are absent; source can be rechecked")
            : Decision(
                  ReconcileAction::kInconsistent,
                  "receipt exists but files and source are unavailable");
    }
    return Decision(
        ReconcileAction::kInconsistent,
        "observed temp is missing or does not match the verified receipt");
}

}  // namespace photobridge
