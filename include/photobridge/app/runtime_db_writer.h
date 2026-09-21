#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

#include "photobridge/app/migration_runtime_store.h"
#include "photobridge/app/sqlite_connection.h"
#include "photobridge/app/task_runtime_repository.h"

namespace photobridge {

struct RuntimeDbWriterStats {
    std::uint64_t accepted_commands = 0;
    std::uint64_t commands = 0;
    std::uint64_t transaction_groups = 0;
    std::uint64_t batched_commands = 0;
    std::size_t largest_batch = 0;
};

// Serializes all database writes made during the concurrent migrate phase.
// Each public call returns only after its repository transaction has completed.
class RuntimeDbWriter final : public MigrationRuntimeStore {
public:
    static StatusOr<std::unique_ptr<RuntimeDbWriter>> Start(
        const std::filesystem::path& database_path,
        std::size_t max_batch_size = 8);
    ~RuntimeDbWriter() override;

    RuntimeDbWriter(const RuntimeDbWriter&) = delete;
    RuntimeDbWriter& operator=(const RuntimeDbWriter&) = delete;

    void Stop();
    RuntimeDbWriterStats stats() const noexcept;

    StatusOr<ClaimedTask> ClaimNextReady(
        const std::string& plan_id, ExecutionEpoch epoch) override;
    Status MarkCommitIntent(
        const std::string& plan_id, const TaskId& task_id,
        ExecutionEpoch epoch, const std::string& attempt_id) override;
    Status MarkTempWritten(
        const std::string& plan_id, const TaskId& task_id,
        ExecutionEpoch epoch, const std::string& attempt_id) override;
    Status PersistVerifiedReceipt(
        const std::string& plan_id, const VerifiedReceipt& receipt) override;
    Status MarkRetryable(
        const std::string& plan_id, const TaskId& task_id,
        ExecutionEpoch epoch, const std::string& attempt_id,
        const Status& error) override;

private:
    struct Claim {
        std::string plan_id;
        ExecutionEpoch epoch;
        std::promise<StatusOr<ClaimedTask>> completion;
    };
    struct CommitIntent {
        std::string plan_id, task_id, attempt_id;
        ExecutionEpoch epoch;
        std::promise<Status> completion;
    };
    struct TempWritten {
        std::string plan_id, task_id, attempt_id;
        ExecutionEpoch epoch;
        std::promise<Status> completion;
    };
    struct Receipt {
        std::string plan_id;
        VerifiedReceipt receipt;
        std::promise<Status> completion;
    };
    struct Retryable {
        std::string plan_id, task_id, attempt_id;
        ExecutionEpoch epoch;
        Status error;
        std::promise<Status> completion;
    };
    using Command = std::variant<Claim, CommitIntent, TempWritten, Receipt, Retryable>;

    RuntimeDbWriter(SqliteConnection connection, std::size_t max_batch_size);
    Status Enqueue(Command command);
    void Run();
    void Execute(Command& command);
    Status ExecuteBatchedOperation(Command& command);
    void ExecuteBatch(std::vector<Command>& commands);
    static void Fail(Command& command, const Status& error);
    static void Complete(Command& command, const Status& status);

    SqliteConnection connection_;
    TaskRuntimeRepository repository_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Command> commands_;
    std::size_t max_batch_size_ = 8;
    bool stopping_ = false;
    Status fatal_ = Status::Ok();
    std::atomic<std::uint64_t> command_count_{0};
    std::atomic<std::uint64_t> accepted_command_count_{0};
    std::atomic<std::uint64_t> transaction_group_count_{0};
    std::atomic<std::uint64_t> batched_command_count_{0};
    std::atomic<std::size_t> largest_batch_{0};
};

}  // namespace photobridge
#include <atomic>
