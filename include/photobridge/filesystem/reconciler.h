#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "photobridge/common/digest.h"
#include "photobridge/common/status_or.h"
#include "photobridge/model/file_identity.h"
#include "photobridge/model/task.h"

namespace photobridge {

struct CommitIntent {
    TaskId task_id;
    std::string attempt_id;
    ExecutionEpoch owner_epoch;
    std::string temp_path;
    std::string final_path;
};

struct VerifiedReceipt {
    TaskId task_id;
    std::string attempt_id;
    ExecutionEpoch owner_epoch;
    std::string temp_path;
    std::string final_path;
    std::uint64_t content_size = 0;
    std::optional<Digest> source_digest;
    Digest target_digest;
    std::optional<FileIdentity> source_identity;
};

struct ObservedFileState {
    bool source_available = false;
    bool temp_exists = false;
    bool final_exists = false;
    std::optional<FileIdentity> source_identity;
    std::optional<FileIdentity> temp_identity;
    std::optional<FileIdentity> final_identity;
    std::uint64_t temp_size = 0;
    std::uint64_t final_size = 0;
    std::optional<Digest> temp_digest;
    std::optional<Digest> final_digest;
};

enum class ReconcileAction {
    kRetryTask,
    kResumeCommitFromTemp,
    kAdoptFinal,
    kAdoptFinalAndCleanupTemp,
    kReverifyCommitted,
    kTargetConflict,
    kInconsistent,
};

struct ReconcileDecision {
    ReconcileAction action;
    std::string reason;
};

// Pure recovery rule evaluation. The caller performs the returned action
// while holding the required locks and persists its result separately.
StatusOr<ReconcileDecision> Decide(
    const TaskSpec& spec,
    const TaskRuntime& runtime,
    const CommitIntent& intent,
    const std::optional<VerifiedReceipt>& receipt,
    const ObservedFileState& observed);

}  // namespace photobridge
