#include "photobridge/model/task.h"

#include <blake3.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace photobridge {
namespace {

void AppendUint64(std::uint64_t value, std::string& output)
{
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output.push_back(static_cast<char>((value >> (index * 8)) & 0xFFU));
    }
}

void AppendLengthPrefixed(std::string_view value, std::string& output)
{
    AppendUint64(static_cast<std::uint64_t>(value.size()), output);
    output.append(value.data(), value.size());
}

void AppendOptionalString(
    const std::optional<std::string>& value,
    std::string& output)
{
    output.push_back(value.has_value() ? '\x01' : '\x00');
    if (value.has_value()) {
        AppendLengthPrefixed(value.value(), output);
    }
}

Digest Blake3(std::string_view payload)
{
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, payload.data(), payload.size());

    Digest digest;
    blake3_hasher_finalize(
        &hasher,
        reinterpret_cast<std::uint8_t*>(digest.bytes.data()),
        digest.bytes.size());
    return digest;
}

Status Invalid(const char* message)
{
    return Status(StatusCode::kInvalidArgument, message);
}

}  // namespace

const char* TaskTypeName(TaskType type) noexcept
{
    switch (type) {
    case TaskType::kMigrateFile: return "MIGRATE_FILE";
    case TaskType::kWriteAssetManifest: return "WRITE_ASSET_MANIFEST";
    case TaskType::kWriteRelationIndex: return "WRITE_RELATION_INDEX";
    case TaskType::kVerifyAsset: return "VERIFY_ASSET";
    case TaskType::kWriteReport: return "WRITE_REPORT";
    }
    return "UNKNOWN";
}

bool IsKnownTaskType(TaskType type) noexcept
{
    return TaskTypeName(type)[0] != 'U';
}

Status ValidateTaskSpec(const TaskSpec& task)
{
    if (task.id.empty()) {
        return Invalid("task id must not be empty");
    }
    if (task.task_key.empty()) {
        return Invalid("task key must not be empty");
    }
    if (!IsKnownTaskType(task.type)) {
        return Invalid("task type is unknown");
    }
    if (task.asset_id.empty()) {
        return Invalid("task asset id must not be empty");
    }
    if (task.target_path.bytes().empty()) {
        return Invalid("task target path must not be empty");
    }
    if (task.id.find('\0') != std::string::npos
        || task.task_key.find('\0') != std::string::npos
        || task.asset_id.find('\0') != std::string::npos) {
        return Invalid("task identifiers must not contain NUL");
    }
    if (task.source_asset_id.has_value()
        && (task.source_asset_id.value().empty()
            || task.source_asset_id.value().find('\0') != std::string::npos)) {
        return Invalid("task source asset id must be non-empty and NUL-free");
    }
    if (task.type == TaskType::kMigrateFile
        && !task.source_asset_id.has_value()) {
        return Invalid("file migration task requires a source asset id");
    }
    return Status::Ok();
}

Status ValidateTaskRuntime(const TaskRuntime& runtime)
{
    if (runtime.id.empty()) {
        return Invalid("task runtime id must not be empty");
    }
    if (runtime.id.find('\0') != std::string::npos) {
        return Invalid("task runtime id must not contain NUL");
    }
    if (runtime.attempt_id.has_value()
        && (runtime.attempt_id.value().empty()
            || runtime.attempt_id.value().find('\0') != std::string::npos)) {
        return Invalid("task attempt id must be non-empty and NUL-free");
    }
    if (runtime.state == TaskState::kRunning
        && (runtime.owner_epoch.value == 0 || !runtime.attempt_id.has_value())) {
        return Invalid(
            "running task runtime requires an owner epoch and attempt id");
    }
    return Status::Ok();
}

StatusOr<std::string> TaskKeyFor(const TaskSpec& task)
{
    if (!IsKnownTaskType(task.type)) {
        return Invalid("task type is unknown");
    }
    if (task.asset_id.empty() || task.target_path.bytes().empty()) {
        return Invalid("task key requires an asset id and target path");
    }
    if (task.source_asset_id.has_value()
        && task.source_asset_id.value().empty()) {
        return Invalid("task key source asset id must not be empty");
    }

    std::string payload;
    payload.reserve(64 + task.asset_id.size() + task.target_path.bytes().size());
    payload.append(
        kTaskKeySemanticVersion.data(),
        kTaskKeySemanticVersion.size());
    AppendLengthPrefixed(task.asset_id, payload);
    AppendLengthPrefixed(TaskTypeName(task.type), payload);
    AppendLengthPrefixed(task.target_path.bytes(), payload);
    AppendOptionalString(task.source_asset_id, payload);

    return "task-" + Blake3(payload).ToHex();
}

bool IsValidTransition(TaskState from, TaskState to) noexcept
{
    switch (from) {
    case TaskState::kPlanned:
        return to == TaskState::kReady || to == TaskState::kSkipped;
    case TaskState::kReady:
        return to == TaskState::kRunning || to == TaskState::kSkipped;
    case TaskState::kRunning:
        return to == TaskState::kSucceeded
            || to == TaskState::kRetryable
            || to == TaskState::kFailed
            || to == TaskState::kNeedsReview
            || to == TaskState::kInconsistent;
    case TaskState::kRetryable:
        return to == TaskState::kReady;
    case TaskState::kSucceeded:
    case TaskState::kFailed:
    case TaskState::kNeedsReview:
    case TaskState::kInconsistent:
    case TaskState::kSkipped:
        return false;
    }
    return false;
}

bool IsValidTransition(FileAttemptState from, FileAttemptState to) noexcept
{
    switch (from) {
    case FileAttemptState::kPlanned:
        return to == FileAttemptState::kReady
            || to == FileAttemptState::kSkipped;
    case FileAttemptState::kReady:
        return to == FileAttemptState::kRunning
            || to == FileAttemptState::kSkipped;
    case FileAttemptState::kRunning:
        return to == FileAttemptState::kCommitIntent
            || to == FileAttemptState::kRetryable
            || to == FileAttemptState::kFailed
            || to == FileAttemptState::kNeedsReview
            || to == FileAttemptState::kInconsistent;
    case FileAttemptState::kCommitIntent:
        return to == FileAttemptState::kTempWritten
            || to == FileAttemptState::kRetryable
            || to == FileAttemptState::kFailed
            || to == FileAttemptState::kNeedsReview
            || to == FileAttemptState::kInconsistent;
    case FileAttemptState::kTempWritten:
        return to == FileAttemptState::kVerifiedDurable
            || to == FileAttemptState::kRetryable
            || to == FileAttemptState::kFailed
            || to == FileAttemptState::kNeedsReview
            || to == FileAttemptState::kInconsistent;
    case FileAttemptState::kVerifiedDurable:
        return to == FileAttemptState::kCommitted
            || to == FileAttemptState::kRetryable
            || to == FileAttemptState::kFailed
            || to == FileAttemptState::kNeedsReview
            || to == FileAttemptState::kInconsistent;
    case FileAttemptState::kRetryable:
        return to == FileAttemptState::kReady;
    case FileAttemptState::kCommitted:
    case FileAttemptState::kFailed:
    case FileAttemptState::kNeedsReview:
    case FileAttemptState::kInconsistent:
    case FileAttemptState::kSkipped:
        return false;
    }
    return false;
}

}  // namespace photobridge
