#pragma once

#include <cstddef>
#include <compare>
#include <string>
#include <utility>
#include <vector>

#include "photobridge/common/status.h"
#include "photobridge/model/task.h"

namespace photobridge {

struct TaskDependency {
    TaskId task;
    TaskId depends_on;

    auto operator<=>(const TaskDependency&) const = default;
};

class TaskGraph final {
public:
    // Tasks are stored by id. Adding a task with a duplicate id or task key
    // is rejected before the graph can be frozen.
    Status AddTask(TaskSpec task);
    Status AddDependency(TaskId task, TaskId depends_on);

    // Validates the complete graph with deterministic Kahn traversal.
    Status ValidateAcyclic() const;

    // Returns tasks with no dependencies in canonical task-id order.
    std::vector<TaskId> InitialReadyTasks() const;

    // These accessors expose the stable serialization order used by a later
    // plan artifact adapter: task id first, then dependency endpoints.
    std::vector<TaskSpec> Tasks() const;
    std::vector<TaskDependency> Dependencies() const;

    std::size_t size() const noexcept;

private:
    std::vector<TaskSpec> tasks_;
    std::vector<TaskDependency> dependencies_;
};

}  // namespace photobridge
