#include "photobridge/model/task_graph.h"

#include <algorithm>
#include <map>
#include <set>

namespace photobridge {
namespace {

Status Invalid(const char* message)
{
    return Status(StatusCode::kInvalidArgument, message);
}

const TaskSpec* FindTask(
    const std::vector<TaskSpec>& tasks,
    const TaskId& id) noexcept
{
    for (const TaskSpec& task : tasks) {
        if (task.id == id) {
            return &task;
        }
    }
    return nullptr;
}

bool HasDependency(
    const std::vector<TaskDependency>& dependencies,
    const TaskDependency& candidate) noexcept
{
    return std::find(
        dependencies.begin(),
        dependencies.end(),
        candidate) != dependencies.end();
}

}  // namespace

Status TaskGraph::AddTask(TaskSpec task)
{
    Status status = ValidateTaskSpec(task);
    if (!status.ok()) {
        return status;
    }
    if (FindTask(tasks_, task.id) != nullptr) {
        return Status(
            StatusCode::kAlreadyExists,
            "task graph task id already exists");
    }
    for (const TaskSpec& existing : tasks_) {
        if (existing.task_key == task.task_key) {
            return Status(
                StatusCode::kAlreadyExists,
                "task graph task key already exists");
        }
    }
    tasks_.push_back(std::move(task));
    return Status::Ok();
}

Status TaskGraph::AddDependency(TaskId task, TaskId depends_on)
{
    if (task.empty() || depends_on.empty()) {
        return Invalid("task dependency ids must not be empty");
    }
    if (task == depends_on) {
        return Invalid("task graph must not contain a self dependency");
    }
    if (FindTask(tasks_, task) == nullptr
        || FindTask(tasks_, depends_on) == nullptr) {
        return Status(
            StatusCode::kNotFound,
            "task dependency references an unknown task");
    }

    TaskDependency dependency{std::move(task), std::move(depends_on)};
    if (HasDependency(dependencies_, dependency)) {
        return Status(
            StatusCode::kAlreadyExists,
            "task dependency already exists");
    }
    dependencies_.push_back(std::move(dependency));
    return Status::Ok();
}

Status TaskGraph::ValidateAcyclic() const
{
    std::map<TaskId, std::size_t> indegree;
    std::map<TaskId, std::vector<TaskId>> outgoing;
    for (const TaskSpec& task : tasks_) {
        indegree.emplace(task.id, 0U);
        outgoing.emplace(task.id, std::vector<TaskId>{});
    }

    for (const TaskDependency& dependency : dependencies_) {
        auto task = indegree.find(dependency.task);
        auto depends_on = indegree.find(dependency.depends_on);
        if (task == indegree.end() || depends_on == indegree.end()) {
            return Status(
                StatusCode::kInvalidArgument,
                "task graph contains an unknown dependency reference");
        }
        ++task->second;
        outgoing[dependency.depends_on].push_back(dependency.task);
    }

    std::set<TaskId> ready;
    for (const auto& [id, count] : indegree) {
        if (count == 0U) {
            ready.insert(id);
        }
    }

    std::size_t visited = 0;
    while (!ready.empty()) {
        const TaskId id = *ready.begin();
        ready.erase(ready.begin());
        ++visited;

        std::vector<TaskId>& dependents = outgoing[id];
        std::sort(dependents.begin(), dependents.end());
        for (const TaskId& dependent : dependents) {
            auto count = indegree.find(dependent);
            --count->second;
            if (count->second == 0U) {
                ready.insert(dependent);
            }
        }
    }

    if (visited != tasks_.size()) {
        return Invalid("task graph contains a dependency cycle");
    }
    return Status::Ok();
}

std::vector<TaskId> TaskGraph::InitialReadyTasks() const
{
    std::set<TaskId> ids;
    for (const TaskSpec& task : tasks_) {
        ids.insert(task.id);
    }
    for (const TaskDependency& dependency : dependencies_) {
        ids.erase(dependency.task);
    }
    return std::vector<TaskId>(ids.begin(), ids.end());
}

std::vector<TaskSpec> TaskGraph::Tasks() const
{
    std::vector<TaskSpec> result = tasks_;
    std::sort(
        result.begin(),
        result.end(),
        [](const TaskSpec& lhs, const TaskSpec& rhs) {
            return lhs.id < rhs.id;
        });
    return result;
}

std::vector<TaskDependency> TaskGraph::Dependencies() const
{
    std::vector<TaskDependency> result = dependencies_;
    std::sort(result.begin(), result.end());
    return result;
}

std::size_t TaskGraph::size() const noexcept
{
    return tasks_.size();
}

}  // namespace photobridge
