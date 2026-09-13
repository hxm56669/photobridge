#include <chrono>
#include <future>

#include <gtest/gtest.h>

#include "photobridge/app/fd_permit_pool.h"

TEST(FdPermitPoolTest, LimitsConcurrentDescriptorPermits)
{
    photobridge::FdPermitPool pool(1);
    ASSERT_TRUE(pool.Acquire().ok());
    auto waiting = std::async(std::launch::async, [&pool] {
        return pool.Acquire();
    });
    EXPECT_EQ(
        waiting.wait_for(std::chrono::milliseconds(20)),
        std::future_status::timeout);
    pool.Release();
    EXPECT_TRUE(waiting.get().ok());
    EXPECT_EQ(pool.in_use(), 1U);
    pool.Release();
    EXPECT_EQ(pool.in_use(), 0U);
}

TEST(FdPermitPoolTest, CloseRejectsFutureOpens)
{
    photobridge::FdPermitPool pool(2);
    pool.Close();
    EXPECT_EQ(pool.Acquire().code(), photobridge::StatusCode::kInvalidArgument);
    pool.Release();
    EXPECT_EQ(pool.in_use(), 0U);
}

TEST(FdPermitPoolTest, RejectsZeroCapacityWithoutWaiting)
{
    photobridge::FdPermitPool pool(0);
    EXPECT_EQ(
        pool.Acquire().code(),
        photobridge::StatusCode::kInvalidArgument);
}
