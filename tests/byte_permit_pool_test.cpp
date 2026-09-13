#include <future>
#include <chrono>

#include <gtest/gtest.h>

#include "photobridge/app/byte_permit_pool.h"

TEST(BytePermitPoolTest, RejectsRequestLargerThanCapacity)
{
    photobridge::BytePermitPool pool(10);
    EXPECT_EQ(pool.Acquire(11).code(), photobridge::StatusCode::kInvalidArgument);
    EXPECT_TRUE(pool.Acquire(10).ok());
    EXPECT_EQ(pool.in_use(), 10U);
    pool.Release(10);
    EXPECT_EQ(pool.in_use(), 0U);
}

TEST(BytePermitPoolTest, WaitsForReleaseAndTracksUsage)
{
    photobridge::BytePermitPool pool(10);
    ASSERT_TRUE(pool.Acquire(8).ok());
    auto waiting = std::async(std::launch::async, [&pool] {
        return pool.Acquire(4);
    });
    EXPECT_EQ(
        waiting.wait_for(std::chrono::milliseconds(20)),
        std::future_status::timeout);
    pool.Release(8);
    ASSERT_TRUE(waiting.get().ok());
    EXPECT_EQ(pool.in_use(), 4U);
}

TEST(BytePermitPoolTest, CloseWakesWaiters)
{
    photobridge::BytePermitPool pool(1);
    ASSERT_TRUE(pool.Acquire(1).ok());
    auto waiting = std::async(std::launch::async, [&pool] {
        return pool.Acquire(1);
    });
    pool.Close();
    EXPECT_EQ(waiting.get().code(), photobridge::StatusCode::kInvalidArgument);
    pool.Release(1);
}
