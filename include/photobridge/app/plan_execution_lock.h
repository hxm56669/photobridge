#pragma once

#include <filesystem>

#include "photobridge/common/status_or.h"
#include "photobridge/common/unique_fd.h"

namespace photobridge {

class PlanExecutionLock final {
public:
    static StatusOr<PlanExecutionLock> Acquire(
        const std::filesystem::path& lock_path);

    PlanExecutionLock(const PlanExecutionLock&) = delete;
    PlanExecutionLock& operator=(const PlanExecutionLock&) = delete;
    PlanExecutionLock(PlanExecutionLock&&) noexcept = default;
    PlanExecutionLock& operator=(PlanExecutionLock&&) noexcept = default;

private:
    explicit PlanExecutionLock(UniqueFd fd) noexcept;

    UniqueFd fd_;
};

}  // namespace photobridge
