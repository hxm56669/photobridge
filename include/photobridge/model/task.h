#pragma once

#include <cstdint>
#include <compare>
#include <optional>
#include <string>
#include <string_view>

#include "photobridge/common/digest.h"
#include "photobridge/common/status.h"
#include "photobridge/common/status_or.h"
#include "photobridge/model/logical_asset.h"
#include "photobridge/model/relative_path.h"

namespace photobridge {

using TaskId = std::string;

inline constexpr std::string_view kTaskKeySemanticVersion = "PB_TASK_V1";

struct ExecutionEpoch {
    std::uint64_t value = 0;

    auto operator<=>(const ExecutionEpoch&) const = default;
};

enum class TaskType {
    kMigrateFile,
    kWriteAssetManifest,
    kWriteRelationIndex,
    kVerifyAsset,
    kWriteReport,
};

const char* TaskTypeName(TaskType type) noexcept;
bool IsKnownTaskType(TaskType type) noexcept;

enum class TaskState {
    kPlanned,
    kReady,
    kRunning,
    kSucceeded,
    kRetryable,
    kFailed,
    kNeedsReview,
    kInconsistent,
    kSkipped,
};

enum class FileAttemptState {
    kPlanned,
    kReady,
    kRunning,
    kCommitIntent,
    kTempWritten,
    kVerifiedDurable,
    kCommitted,
    kRetryable,
    kFailed,
    kNeedsReview,
    kInconsistent,
    kSkipped,
};

struct TaskSpec {
    TaskId id;
    std::string task_key;
    TaskType type = TaskType::kMigrateFile;
    LogicalAssetId asset_id;
    std::optional<PhysicalAssetId> source_asset_id;
    RelativePath target_path;
    std::uint64_t estimated_bytes = 0;
    std::optional<Digest> expected_digest;
};

struct TaskRuntime {
    TaskId id;
    TaskState state = TaskState::kPlanned;
    ExecutionEpoch owner_epoch;
    std::optional<std::string> attempt_id;
    std::uint32_t attempt_count = 0;
    std::optional<Status> last_error;
};

Status ValidateTaskSpec(const TaskSpec& task);
Status ValidateTaskRuntime(const TaskRuntime& runtime);

// Computes the stable task identity from immutable task semantics. Runtime
// fields, estimated bytes, and expected output digest are intentionally absent.
StatusOr<std::string> TaskKeyFor(const TaskSpec& task);

bool IsValidTransition(TaskState from, TaskState to) noexcept;
bool IsValidTransition(FileAttemptState from, FileAttemptState to) noexcept;

}  // namespace photobridge
