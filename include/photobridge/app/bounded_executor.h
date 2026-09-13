#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "photobridge/common/status.h"

namespace photobridge {

class BoundedExecutor final {
public:
    using Command = std::function<Status()>;

    BoundedExecutor(std::size_t workers, std::size_t max_pending);
    ~BoundedExecutor();

    BoundedExecutor(const BoundedExecutor&) = delete;
    BoundedExecutor& operator=(const BoundedExecutor&) = delete;

    std::future<Status> Submit(Command command);
    void Stop() noexcept;

private:
    struct Item {
        Command command;
        std::promise<Status> result;
    };

    void Run();

    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<Item> pending_;
    const std::size_t max_pending_;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

}  // namespace photobridge
