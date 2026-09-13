#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/app/bounded_executor.h"

namespace {

std::vector<int> RunTasks(std::size_t workers)
{
    photobridge::BoundedExecutor executor(workers, 128);
    std::vector<std::future<photobridge::Status>> futures;
    futures.reserve(64);
    for (int index = 0; index < 64; ++index) {
        futures.push_back(executor.Submit([index] {
            return index % 3 == 0
                ? photobridge::Status::Ok()
                : photobridge::Status(
                    photobridge::StatusCode::kInvalidArgument,
                    std::to_string(index));
        }));
    }
    std::vector<int> results;
    results.reserve(futures.size());
    for (auto& future : futures) {
        results.push_back(future.get().ok() ? 1 : 0);
    }
    executor.Stop();
    return results;
}

}  // namespace

TEST(BoundedExecutorTest, OneTwoAndFourWorkersHaveTheSameTaskResults)
{
    EXPECT_EQ(RunTasks(1), RunTasks(2));
    EXPECT_EQ(RunTasks(2), RunTasks(4));
}

TEST(BoundedExecutorTest, EnforcesQueueCapacityAndStop)
{
    photobridge::BoundedExecutor executor(1, 1);
    auto started = std::make_shared<std::promise<void>>();
    std::promise<void> release;
    auto gate = std::make_shared<std::future<void>>(release.get_future());
    auto first = executor.Submit([gate, started] {
        started->set_value();
        gate->wait();
        return photobridge::Status::Ok();
    });
    started->get_future().wait();
    auto second = executor.Submit([] { return photobridge::Status::Ok(); });
    auto third = executor.Submit([] { return photobridge::Status::Ok(); });
    EXPECT_EQ(third.get().code(), photobridge::StatusCode::kIoError);
    release.set_value();
    EXPECT_TRUE(first.get().ok());
    EXPECT_TRUE(second.get().ok());
    executor.Stop();
    auto rejected = executor.Submit([] { return photobridge::Status::Ok(); });
    EXPECT_EQ(rejected.get().code(), photobridge::StatusCode::kInvalidArgument);
}
