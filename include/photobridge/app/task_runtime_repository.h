#pragma once

#include <string>

#include "photobridge/app/sqlite_connection.h"
#include "photobridge/app/sqlite_statement.h"
#include "photobridge/common/status.h"
#include "photobridge/common/status_or.h"
#include "photobridge/filesystem/reconciler.h"
#include "photobridge/model/task.h"

namespace photobridge {

struct ClaimedTask {
    TaskId id;
    TaskRuntime runtime;
};

class TaskRuntimeRepository final {
public:
    explicit TaskRuntimeRepository(SqliteConnection& connection) noexcept;

    Status AddTask(const std::string& plan_id, const TaskSpec& task);
    Status AddDependency(
        const std::string& plan_id,
        const TaskId& task,
        const TaskId& depends_on);
    Status SetReady(const std::string& plan_id, const TaskId& task_id);

    StatusOr<ExecutionEpoch> AcquireNextExecutionEpoch(
        const std::string& plan_id);
    StatusOr<ExecutionEpoch> ReadCurrentEpoch(
        const std::string& plan_id) const;

    StatusOr<ClaimedTask> ClaimNextReady(
        const std::string& plan_id,
        ExecutionEpoch epoch);
    StatusOr<ClaimedTask> ClaimNextReady(
        const std::string& plan_id,
        ExecutionEpoch epoch,
        const std::string& attempt_id);

    Status MarkCommitIntent(
        const std::string& plan_id,
        const TaskId& task_id,
        ExecutionEpoch epoch,
        const std::string& attempt_id);
    Status MarkTempWritten(
        const std::string& plan_id,
        const TaskId& task_id,
        ExecutionEpoch epoch,
        const std::string& attempt_id);

    Status MarkSucceeded(
        const std::string& plan_id,
        const TaskId& task_id,
        ExecutionEpoch epoch,
        const std::string& attempt_id);
    Status MarkRetryable(
        const std::string& plan_id,
        const TaskId& task_id,
        ExecutionEpoch epoch,
        const std::string& attempt_id,
        const Status& error);

    Status RecoverSucceeded(
        const std::string& plan_id,
        const TaskId& task_id,
        ExecutionEpoch recovery_epoch,
        ExecutionEpoch expected_old_epoch,
        const std::string& expected_old_attempt_id,
        const std::string& reason);
    Status RecoverRetryable(
        const std::string& plan_id,
        const TaskId& task_id,
        ExecutionEpoch recovery_epoch,
        ExecutionEpoch expected_old_epoch,
        const std::string& expected_old_attempt_id,
        const std::string& reason);
    Status RecoverInconsistent(
        const std::string& plan_id,
        const TaskId& task_id,
        ExecutionEpoch recovery_epoch,
        ExecutionEpoch expected_old_epoch,
        const std::string& expected_old_attempt_id,
        const std::string& reason);

    Status PersistVerifiedReceipt(
        const std::string& plan_id,
        const VerifiedReceipt& receipt);

    StatusOr<VerifiedReceipt> ReadVerifiedReceipt(
        const std::string& plan_id,
        const TaskId& task_id,
        const std::string& attempt_id) const;

    StatusOr<TaskRuntime> ReadRuntime(
        const std::string& plan_id,
        const TaskId& task_id) const;

private:
    StatusOr<ClaimedTask> ClaimNextReadyImpl(
        const std::string& plan_id,
        ExecutionEpoch epoch,
        const std::string* supplied_attempt_id);
    SqliteConnection* connection_;
    SqliteStatement insert_task_;
    SqliteStatement check_dependency_cycle_;
    SqliteStatement insert_dependency_;
    SqliteStatement set_ready_;
    SqliteStatement insert_event_;
    SqliteStatement update_attempt_;
    mutable SqliteStatement read_runtime_;
    mutable SqliteStatement read_current_epoch_;
    mutable SqliteStatement check_task_ownership_;
    SqliteStatement read_attempt_state_;
    SqliteStatement update_epoch_;
    SqliteStatement claim_next_ready_;
    SqliteStatement claim_task_;
    SqliteStatement insert_attempt_;
    SqliteStatement finish_task_;
    SqliteStatement recover_task_;
    SqliteStatement insert_receipt_;
    mutable SqliteStatement read_receipt_;
};

}  // namespace photobridge
