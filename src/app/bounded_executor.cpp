#include "photobridge/app/bounded_executor.h"

#include <algorithm>

namespace photobridge {

BoundedExecutor::BoundedExecutor(
    std::size_t workers,
    std::size_t max_pending)
    : max_pending_(max_pending == 0 ? 1 : max_pending)
{
    workers = std::max<std::size_t>(workers, 1);
    workers_.reserve(workers);
    for (std::size_t index = 0; index < workers; ++index) {
        workers_.emplace_back(&BoundedExecutor::Run, this);
    }
}

BoundedExecutor::~BoundedExecutor()
{
    Stop();
}

std::future<Status> BoundedExecutor::Submit(Command command)
{
    Item item{std::move(command), std::promise<Status>()};
    std::future<Status> result = item.result.get_future();
    {
        std::lock_guard lock(mutex_);
        if (stopping_) {
            item.result.set_value(Status(
                StatusCode::kInvalidArgument,
                "bounded executor is stopped"));
            return result;
        }
        if (pending_.size() >= max_pending_) {
            item.result.set_value(Status(
                StatusCode::kIoError,
                "bounded executor queue is full"));
            return result;
        }
        pending_.push(std::move(item));
    }
    condition_.notify_one();
    return result;
}

void BoundedExecutor::Stop() noexcept
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

void BoundedExecutor::Run()
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
            "bounded executor command did not run");
        try {
            status = item.command();
        } catch (...) {
            status = Status(
                StatusCode::kInternal,
                "bounded executor command threw an exception");
        }
        item.result.set_value(std::move(status));
    }
}

}  // namespace photobridge
