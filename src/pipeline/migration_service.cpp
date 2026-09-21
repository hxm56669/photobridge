#include "photobridge/pipeline/pipeline_support.h"
#include "photobridge/app/runtime_db_writer.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace photobridge::pipeline {
namespace {

struct QueuedTask {
    ClaimedTask claim;
    std::size_t asset_index;
};

StatusOr<std::string> ExecuteClaimedTask(
    MigrationRuntimeStore& repository,
    const FrozenPlanFile& artifact,
    const MinimalPlanAsset& plan_asset,
    const ClaimedTask& claimed,
    ExecutionEpoch epoch,
    const std::string& source_root,
    std::size_t task_count)
{
    const std::string& plan_id = artifact.plan_id;
    const std::string& attempt_id = claimed.runtime.attempt_id.value();
    const auto retry = [&](const Status& error) -> Status {
        const Status transition = repository.MarkRetryable(
            plan_id, claimed.id, epoch, attempt_id, error);
        return transition.ok() ? error : transition;
    };

    LinuxFileOps file_ops;
    auto source_root_fd = file_ops.OpenRoot(
        std::filesystem::path(source_root), OpenRootMode::kExisting);
    if (!source_root_fd.ok()) return retry(source_root_fd.status());

    const PhysicalAsset source_asset{
        plan_asset.source_path,
        plan_asset.source_identity,
        AssetKind::kUnknown,
        {},
    };
    auto source_guard = MutationGuard::Open(
        file_ops, source_root_fd.value().get(), source_asset);
    if (!source_guard.ok()) return retry(source_guard.status());

    auto target_root_fd = file_ops.OpenRoot(
        std::filesystem::path(artifact.plan.target_root()),
        OpenRootMode::kCreateIfMissing);
    if (!target_root_fd.ok()) return retry(target_root_fd.status());

    const std::string temp_name = TempNameFor(
        plan_id, claimed.id, attempt_id);
    std::array<std::byte, 1024 * 1024> buffer{};
    auto receipt = MigrationAttemptPreparer::Prepare(
        file_ops,
        repository,
        source_guard.value(),
        target_root_fd.value().get(),
        plan_asset,
        plan_id,
        claimed.id,
        epoch,
        attempt_id,
        temp_name,
        std::span<std::byte>(buffer));
    if (!receipt.ok()) return retry(receipt.status());

    std::ostringstream output;
    output << "migrate claimed: " << plan_id
           << " task=" << claimed.id
           << " tasks=" << task_count
           << " source_bytes=" << source_guard.value().manifest_identity().size
           << " temp=" << temp_name
           << " digest=" << receipt.value().source_digest->ToHex()
           << " receipt=VERIFIED_DURABLE state=RUNNING\n";
    return output.str();
}

}  // namespace

class MigrationService final {
public:
    static Status Execute(
        const StatusOr<WorkspaceLayout>& layout,
        const std::string& input_path,
        std::size_t workers,
        CommandContext& context)
    {
        if (workers == 0 || workers > 8) {
            return Status(StatusCode::kInvalidArgument,
                          "migration worker count must be between 1 and 8");
        }
        auto plan_bytes = ReadPlanFile(std::filesystem::path(input_path));
        if (!plan_bytes.ok()) return plan_bytes.status();
        auto artifact = ReadFrozenPlan(plan_bytes.value());
        if (!artifact.ok()) return artifact.status();
        if (artifact.value().plan.assets().empty()) {
            return Status(StatusCode::kInvalidArgument,
                          "migration plan must contain at least one asset");
        }
        auto plan_lock = PlanExecutionLock::Acquire(
            layout.value().locks / (artifact.value().plan_id + ".lock"));
        if (!plan_lock.ok()) return plan_lock.status();
        auto connection = SqliteConnection::Open(layout.value().database);
        if (!connection.ok()) return connection.status();
        Status status = EnsureSchema(connection.value());
        if (!status.ok()) return status;

        std::size_t task_count = 0;
        status = MaterializePlan(connection.value(), artifact.value(),
                                 std::filesystem::path(input_path), task_count);
        if (!status.ok()) return status;

        TaskRuntimeRepository repository(connection.value());
        auto state_counts = ReadTaskStateCounts(
            connection.value(), artifact.value().plan_id);
        if (!state_counts.ok()) return state_counts.status();
        if (state_counts.value()[static_cast<std::size_t>(TaskState::kRunning)] != 0) {
            return Status(StatusCode::kInvalidArgument,
                          "migrate found RUNNING task; resume is required before migrate");
        }

        std::unordered_map<std::string, std::size_t> asset_indices;
        bool ready_found = false;
        for (std::size_t index = 0; index < artifact.value().plan.assets().size(); ++index) {
            auto task = TaskSpecForPlanAsset(artifact.value().plan.assets()[index]);
            if (!task.ok()) return task.status();
            asset_indices.emplace(task.value().id, index);
            auto runtime = repository.ReadRuntime(
                artifact.value().plan_id, task.value().id);
            if (!runtime.ok()) return runtime.status();
            if (runtime.value().state == TaskState::kRetryable) {
                status = repository.SetReady(artifact.value().plan_id, task.value().id);
                if (!status.ok()) return status;
            }
            ready_found |= runtime.value().state == TaskState::kReady
                || runtime.value().state == TaskState::kRetryable;
        }
        if (!ready_found) {
            return Status(StatusCode::kNotFound,
                          "no READY task remains in the materialized plan");
        }

        auto epoch_result = repository.AcquireNextExecutionEpoch(
            artifact.value().plan_id);
        if (!epoch_result.ok()) return epoch_result.status();
        const ExecutionEpoch epoch = epoch_result.value();
        auto source_root = ReadManifestSourceRoot(
            connection.value(), artifact.value().plan.source_manifest_id());
        if (!source_root.ok()) return source_root.status();

        auto writer = RuntimeDbWriter::Start(layout.value().database);
        if (!writer.ok()) return writer.status();

        constexpr std::size_t queue_capacity = 8;
        std::deque<QueuedTask> queue;
        std::mutex mutex;
        std::condition_variable queue_changed;
        bool producer_done = false;
        Status first_error = Status::Ok();
        std::vector<std::string> messages;
        std::vector<std::thread> pool;
        pool.reserve(workers);
        for (std::size_t index = 0; index < workers; ++index) {
            pool.emplace_back([&] {
                while (true) {
                    QueuedTask item;
                    {
                        std::unique_lock lock(mutex);
                        queue_changed.wait(lock, [&] {
                            return producer_done || !queue.empty();
                        });
                        if (queue.empty()) return;
                        item = std::move(queue.front());
                        queue.pop_front();
                        queue_changed.notify_all();
                    }
                    auto result = ExecuteClaimedTask(
                        *writer.value(), artifact.value(),
                        artifact.value().plan.assets()[item.asset_index],
                        item.claim, epoch, source_root.value(), task_count);
                    std::lock_guard lock(mutex);
                    if (result.ok()) {
                        messages.push_back(std::move(result.value()));
                    } else if (first_error.ok()) {
                        first_error = result.status();
                    }
                    queue_changed.notify_all();
                }
            });
        }

        std::size_t claimed_count = 0;
        Status producer_error = Status::Ok();
        while (true) {
            {
                std::unique_lock lock(mutex);
                queue_changed.wait(lock, [&] {
                    return queue.size() < queue_capacity || !first_error.ok();
                });
                if (!first_error.ok()) break;
            }
            auto claim = writer.value()->ClaimNextReady(
                artifact.value().plan_id, epoch);
            if (!claim.ok()) {
                if (claim.status().code() != StatusCode::kNotFound) {
                    producer_error = claim.status();
                }
                break;
            }
            ++claimed_count;
            const auto found = asset_indices.find(claim.value().id);
            if (found == asset_indices.end()) {
                producer_error = Status(StatusCode::kInternal,
                    "claimed task is missing from frozen plan");
                break;
            }
            {
                std::lock_guard lock(mutex);
                queue.push_back({std::move(claim.value()), found->second});
            }
            queue_changed.notify_all();
        }
        {
            std::lock_guard lock(mutex);
            producer_done = true;
        }
        queue_changed.notify_all();
        for (auto& thread : pool) thread.join();
        writer.value()->Stop();
        for (const auto& message : messages) context.out << message;
        if (!producer_error.ok()) return producer_error;
        if (!first_error.ok()) return first_error;
        if (claimed_count == 0) {
            return Status(StatusCode::kNotFound,
                          "no eligible ready task exists");
        }
        return Status::Ok();
    }
};

Status RunMigrationService(
    const StatusOr<WorkspaceLayout>& layout,
    const std::string& input_path,
    std::size_t workers,
    CommandContext& context)
{
    return MigrationService::Execute(layout, input_path, workers, context);
}

}  // namespace photobridge::pipeline
