#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>

#include "photobridge/app/sqlite_connection.h"

namespace photobridge {

class DbCommandQueue final {
public:
    using Command = std::function<Status(SqliteConnection&)>;

    explicit DbCommandQueue(
        SqliteConnection& connection,
        std::size_t max_pending = 64);
    ~DbCommandQueue();

    DbCommandQueue(const DbCommandQueue&) = delete;
    DbCommandQueue& operator=(const DbCommandQueue&) = delete;

    std::future<Status> Submit(Command command);
    void Stop() noexcept;

private:
    struct Item {
        Command command;
        std::promise<Status> result;
    };

    void Run();

    SqliteConnection* connection_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<Item> pending_;
    std::size_t max_pending_;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace photobridge
