#pragma once

#include <string>
#include <span>

#include "photobridge/app/sqlite_connection.h"
#include "photobridge/app/task_runtime_repository.h"
#include "photobridge/common/status_or.h"
#include "photobridge/filesystem/mutation_guard.h"
#include "photobridge/model/canonical_plan.h"

namespace photobridge {

class MigrationAttemptPreparer final {
public:
    static StatusOr<VerifiedReceipt> Prepare(
        FileOps& file_ops,
        TaskRuntimeRepository& repository,
        MutationGuard& source_guard,
        int target_root_fd,
        const MinimalPlanAsset& plan_asset,
        const std::string& plan_id,
        const std::string& task_id,
        ExecutionEpoch execution_epoch,
        const std::string& attempt_id,
        const std::string& temp_name,
        std::span<std::byte> buffer);
};

}  // namespace photobridge
