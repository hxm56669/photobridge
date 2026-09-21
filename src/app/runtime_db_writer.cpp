#include "photobridge/app/runtime_db_writer.h"

#include <exception>
#include <type_traits>
#include <utility>

namespace photobridge {
namespace {

Status StoppedStatus()
{
    return Status(StatusCode::kInternal, "runtime DB writer is stopped");
}

}  // namespace

RuntimeDbWriter::RuntimeDbWriter(SqliteConnection connection)
    : connection_(std::move(connection)), repository_(connection_)
{
}

StatusOr<std::unique_ptr<RuntimeDbWriter>> RuntimeDbWriter::Start(
    const std::filesystem::path& database_path)
{
    auto connection = SqliteConnection::Open(database_path);
    if (!connection.ok()) return connection.status();
    auto writer = std::unique_ptr<RuntimeDbWriter>(
        new RuntimeDbWriter(std::move(connection.value())));
    writer->thread_ = std::thread([instance = writer.get()] { instance->Run(); });
    return writer;
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

void RuntimeDbWriter::Fail(Command& command, const Status& error)
{
    std::visit([&error](auto& item) { item.completion.set_value(error); }, command);
}

void RuntimeDbWriter::Run()
{
    while (true) {
        Command command;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] {
                return stopping_ || !commands_.empty();
            });
            if (commands_.empty()) return;
            command = std::move(commands_.front());
            commands_.pop_front();
        }
        try {
            Execute(command);
        } catch (...) {
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
            Fail(command, failure);
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
