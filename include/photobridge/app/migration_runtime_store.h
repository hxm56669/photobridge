#pragma once

#include <string>

#include "photobridge/common/status_or.h"
#include "photobridge/filesystem/reconciler.h"
#include "photobridge/model/task.h"

namespace photobridge {

struct ClaimedTask {
    TaskId id;
    TaskRuntime runtime;
};

// Operations used while a migrate command is processing tasks concurrently.
class MigrationRuntimeStore {
public:
    virtual ~MigrationRuntimeStore() = default;

    virtual StatusOr<ClaimedTask> ClaimNextReady(
        const std::string& plan_id, ExecutionEpoch epoch) = 0;
    virtual Status MarkCommitIntent(
        const std::string& plan_id, const TaskId& task_id,
        ExecutionEpoch epoch, const std::string& attempt_id) = 0;
    virtual Status MarkTempWritten(
        const std::string& plan_id, const TaskId& task_id,
        ExecutionEpoch epoch, const std::string& attempt_id) = 0;
    virtual Status PersistVerifiedReceipt(
        const std::string& plan_id, const VerifiedReceipt& receipt) = 0;
    virtual Status MarkRetryable(
        const std::string& plan_id, const TaskId& task_id,
        ExecutionEpoch epoch, const std::string& attempt_id,
        const Status& error) = 0;
};

}  // namespace photobridge
