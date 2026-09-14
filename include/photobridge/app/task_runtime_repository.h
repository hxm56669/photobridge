#pragma once

#include <string>

#include "photobridge/app/sqlite_connection.h"
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
    SqliteConnection* connection_;
};

}  // namespace photobridge
