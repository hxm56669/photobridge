#include "photobridge/app/runtime_db_writer.h"

#include <exception>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace photobridge {
namespace {

Status StoppedStatus()
{
    return Status(StatusCode::kInternal, "runtime DB writer is stopped");
}

}  // namespace

RuntimeDbWriter::RuntimeDbWriter(
    SqliteConnection connection,
    std::size_t max_batch_size)
    : connection_(std::move(connection)),
      repository_(connection_),
      max_batch_size_(max_batch_size)
{
}

StatusOr<std::unique_ptr<RuntimeDbWriter>> RuntimeDbWriter::Start(
    const std::filesystem::path& database_path,
    std::size_t max_batch_size)
{
    if (max_batch_size == 0 || max_batch_size > 16) {
        return Status(StatusCode::kInvalidArgument,
                      "runtime DB batch size must be between 1 and 16");
    }
    auto connection = SqliteConnection::Open(database_path);
    if (!connection.ok()) return connection.status();
    auto writer = std::unique_ptr<RuntimeDbWriter>(
        new RuntimeDbWriter(std::move(connection.value()), max_batch_size));
    writer->thread_ = std::thread([instance = writer.get()] { instance->Run(); });
    return writer;
}

RuntimeDbWriterStats RuntimeDbWriter::stats() const noexcept
{
    return RuntimeDbWriterStats{
        accepted_command_count_.load(),
        command_count_.load(),
        transaction_group_count_.load(),
        batched_command_count_.load(),
        largest_batch_.load(),
    };
}

RuntimeDbWriter::~RuntimeDbWriter()
{
    Stop();
}

void RuntimeDbWriter::Stop()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    changed_.notify_one();
    if (thread_.joinable()) thread_.join();
}

Status RuntimeDbWriter::Enqueue(Command command)
{
    {
        std::lock_guard lock(mutex_);
        if (!fatal_.ok()) return fatal_;
        if (stopping_) return StoppedStatus();
        commands_.push_back(std::move(command));
        accepted_command_count_.fetch_add(1);
    }
    changed_.notify_one();
    return Status::Ok();
}

StatusOr<ClaimedTask> RuntimeDbWriter::ClaimNextReady(
    const std::string& plan_id, ExecutionEpoch epoch)
{
    Claim command{plan_id, epoch, {}};
    auto result = command.completion.get_future();
    Status status = Enqueue(std::move(command));
    if (!status.ok()) return status;
    return result.get();
}

Status RuntimeDbWriter::MarkCommitIntent(
    const std::string& plan_id, const TaskId& task_id,
    ExecutionEpoch epoch, const std::string& attempt_id)
{
    CommitIntent command{plan_id, task_id, attempt_id, epoch, {}};
    auto result = command.completion.get_future();
    Status status = Enqueue(std::move(command));
    return status.ok() ? result.get() : status;
}

Status RuntimeDbWriter::MarkTempWritten(
    const std::string& plan_id, const TaskId& task_id,
    ExecutionEpoch epoch, const std::string& attempt_id)
{
    TempWritten command{plan_id, task_id, attempt_id, epoch, {}};
    auto result = command.completion.get_future();
    Status status = Enqueue(std::move(command));
    return status.ok() ? result.get() : status;
}

Status RuntimeDbWriter::PersistVerifiedReceipt(
    const std::string& plan_id, const VerifiedReceipt& receipt)
{
    Receipt command{plan_id, receipt, {}};
    auto result = command.completion.get_future();
    Status status = Enqueue(std::move(command));
    return status.ok() ? result.get() : status;
}

Status RuntimeDbWriter::MarkRetryable(
    const std::string& plan_id, const TaskId& task_id,
    ExecutionEpoch epoch, const std::string& attempt_id, const Status& error)
{
    Retryable command{plan_id, task_id, attempt_id, epoch, error, {}};
    auto result = command.completion.get_future();
    Status status = Enqueue(std::move(command));
    return status.ok() ? result.get() : status;
}

void RuntimeDbWriter::Execute(Command& command)
{
    std::visit([this](auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, Claim>) {
            item.completion.set_value(repository_.ClaimNextReady(
                item.plan_id, item.epoch));
        } else if constexpr (std::is_same_v<T, CommitIntent>) {
            item.completion.set_value(repository_.MarkCommitIntent(
                item.plan_id, item.task_id, item.epoch, item.attempt_id));
        } else if constexpr (std::is_same_v<T, TempWritten>) {
            item.completion.set_value(repository_.MarkTempWritten(
                item.plan_id, item.task_id, item.epoch, item.attempt_id));
        } else if constexpr (std::is_same_v<T, Receipt>) {
            item.completion.set_value(repository_.PersistVerifiedReceipt(
                item.plan_id, item.receipt));
        } else if constexpr (std::is_same_v<T, Retryable>) {
            item.completion.set_value(repository_.MarkRetryable(
                item.plan_id, item.task_id, item.epoch,
                item.attempt_id, item.error));
        }
    }, command);
}

Status RuntimeDbWriter::ExecuteBatchedOperation(Command& command)
{
    return std::visit([this](auto& item) -> Status {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, CommitIntent>) {
            return repository_.MarkCommitIntent(
                item.plan_id, item.task_id, item.epoch, item.attempt_id);
        } else if constexpr (std::is_same_v<T, TempWritten>) {
            return repository_.MarkTempWritten(
                item.plan_id, item.task_id, item.epoch, item.attempt_id);
        } else if constexpr (std::is_same_v<T, Receipt>) {
            return repository_.PersistVerifiedReceipt(
                item.plan_id, item.receipt);
        } else if constexpr (std::is_same_v<T, Retryable>) {
            return repository_.MarkRetryable(
                item.plan_id, item.task_id, item.epoch,
                item.attempt_id, item.error);
        } else {
            return Status(StatusCode::kInternal,
                          "claim command cannot join a runtime write batch");
        }
    }, command);
}

void RuntimeDbWriter::Complete(Command& command, const Status& status)
{
    std::visit([&status](auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, Claim>) {
            item.completion.set_value(status);
        } else {
            item.completion.set_value(status);
        }
    }, command);
}

void RuntimeDbWriter::ExecuteBatch(std::vector<Command>& commands)
{
    command_count_.fetch_add(commands.size());
    transaction_group_count_.fetch_add(1);
    batched_command_count_.fetch_add(commands.size());
    std::size_t observed = largest_batch_.load();
    while (observed < commands.size()
           && !largest_batch_.compare_exchange_weak(observed, commands.size())) {
    }

    const auto batch_identity = [](Command& command) {
        return std::visit([](auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, Receipt>) {
                return std::pair<std::string_view, ExecutionEpoch>{
                    item.plan_id, item.receipt.owner_epoch};
            } else {
                return std::pair<std::string_view, ExecutionEpoch>{
                    item.plan_id, item.epoch};
            }
        }, command);
    };
    const auto [plan_id, epoch] = batch_identity(commands.front());
    Status status = repository_.BeginWriteBatch(std::string(plan_id), epoch);
    if (!status.ok()) {
        for (auto& command : commands) Complete(command, status);
        return;
    }

    std::optional<std::size_t> failed_index;
    Status operation_error = Status::Ok();
    for (std::size_t index = 0; index < commands.size(); ++index) {
        status = ExecuteBatchedOperation(commands[index]);
        if (!status.ok()) {
            failed_index = index;
            operation_error = status;
            break;
        }
    }
    if (failed_index.has_value()) {
        repository_.RollbackWriteBatch();
        const Status rollback_error(
            StatusCode::kInternal,
            "runtime DB batch rolled back: " + operation_error.message());
        for (std::size_t index = 0; index < commands.size(); ++index) {
            Complete(commands[index],
                     index == *failed_index ? operation_error : rollback_error);
        }
        return;
    }

    status = repository_.CommitWriteBatch();
    for (auto& command : commands) Complete(command, status);
}

void RuntimeDbWriter::Fail(Command& command, const Status& error)
{
    std::visit([&error](auto& item) { item.completion.set_value(error); }, command);
}

void RuntimeDbWriter::Run()
{
    while (true) {
        std::vector<Command> batch;
        batch.reserve(max_batch_size_);
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] {
                return stopping_ || !commands_.empty();
            });
            if (commands_.empty()) return;
            batch.push_back(std::move(commands_.front()));
            commands_.pop_front();

            const auto command_key = [](const Command& command)
                -> std::optional<std::tuple<std::size_t, std::string_view,
                                            std::string_view, ExecutionEpoch>> {
                return std::visit([&command](const auto& item)
                    -> std::optional<std::tuple<std::size_t, std::string_view,
                                                std::string_view, ExecutionEpoch>> {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, Claim>) {
                        return std::nullopt;
                    } else if constexpr (std::is_same_v<T, Receipt>) {
                        return std::tuple{command.index(),
                            std::string_view(item.plan_id),
                            std::string_view(item.receipt.task_id),
                            item.receipt.owner_epoch};
                    } else {
                        return std::tuple{command.index(),
                            std::string_view(item.plan_id),
                            std::string_view(item.task_id), item.epoch};
                    }
                }, command);
            };
            const auto first_key = command_key(batch.front());
            if (first_key.has_value() && max_batch_size_ > 1) {
                std::unordered_set<std::string> tasks;
                tasks.emplace(std::string(std::get<2>(*first_key)));
                while (batch.size() < max_batch_size_ && !commands_.empty()) {
                    const auto next_key = command_key(commands_.front());
                    if (!next_key.has_value()
                        || std::get<0>(*next_key) != std::get<0>(*first_key)
                        || std::get<1>(*next_key) != std::get<1>(*first_key)
                        || std::get<3>(*next_key) != std::get<3>(*first_key)
                        || tasks.contains(std::string(std::get<2>(*next_key)))) {
                        break;
                    }
                    tasks.emplace(std::string(std::get<2>(*next_key)));
                    batch.push_back(std::move(commands_.front()));
                    commands_.pop_front();
                }
            }
        }
        try {
            if (batch.size() > 1) {
                ExecuteBatch(batch);
            } else {
                command_count_.fetch_add(1);
                transaction_group_count_.fetch_add(1);
                Execute(batch.front());
            }
        } catch (...) {
            repository_.RollbackWriteBatch();
            std::string message = "runtime DB writer failed";
            try {
                throw;
            } catch (const std::exception& error) {
                message += ": ";
                message += error.what();
            } catch (...) {
                message += " with an unknown exception";
            }
            const Status failure(StatusCode::kInternal, std::move(message));
            for (auto& command : batch) Fail(command, failure);
            std::deque<Command> pending;
            {
                std::lock_guard lock(mutex_);
                fatal_ = failure;
                stopping_ = true;
                pending.swap(commands_);
            }
            for (auto& item : pending) Fail(item, failure);
            return;
        }
    }
}

}  // namespace photobridge
