#include "photobridge/app/db_command_queue.h"

namespace photobridge {

DbCommandQueue::DbCommandQueue(
    SqliteConnection& connection,
    std::size_t max_pending)
    : connection_(&connection),
      max_pending_(max_pending == 0 ? 1 : max_pending),
      worker_(&DbCommandQueue::Run, this)
{
}

DbCommandQueue::~DbCommandQueue()
{
    Stop();
}

std::future<Status> DbCommandQueue::Submit(Command command)
{
    Item item{std::move(command), std::promise<Status>()};
    std::future<Status> result = item.result.get_future();
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            item.result.set_value(Status(
                StatusCode::kInvalidArgument,
                "database command queue is stopped"));
            return result;
        }
        if (pending_.size() >= max_pending_) {
            item.result.set_value(Status(
                StatusCode::kIoError,
                "database command queue is full"));
            return result;
        }
        pending_.push(std::move(item));
    }
    condition_.notify_one();
    return result;
}

void DbCommandQueue::Stop() noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            // A second Stop is still required to be harmless after the
            // worker has already been joined.
        } else {
            stopping_ = true;
        }
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void DbCommandQueue::Run()
{
    for (;;) {
        Item item;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] {
                return stopping_ || !pending_.empty();
            });
            if (pending_.empty()) {
                if (stopping_) return;
                continue;
            }
            item = std::move(pending_.front());
            pending_.pop();
        }
        Status status = Status(
            StatusCode::kInternal,
            "database command did not run");
        try {
            status = item.command(*connection_);
        } catch (...) {
            status = Status(
                StatusCode::kInternal,
                "database command threw an exception");
        }
        item.result.set_value(std::move(status));
    }
}

}  // namespace photobridge
