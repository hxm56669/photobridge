#include <chrono>
#include <filesystem>
#include <memory>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/app/db_command_queue.h"
#include "photobridge/app/sqlite_schema.h"

TEST(DbCommandQueueTest, ExecutesFifoCommandsAndAcknowledgesResults)
{
    const auto path = std::filesystem::temp_directory_path()
        / "photobridge_db_command_queue_test.db";
    std::error_code error;
    std::filesystem::remove(path, error);
    auto connection = photobridge::SqliteConnection::Open(path);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    photobridge::DbCommandQueue queue(connection.value());
    auto first = queue.Submit([](photobridge::SqliteConnection& db) {
        return db.Execute(
            "CREATE TABLE queue_order(value INTEGER NOT NULL);");
    });
    auto second = queue.Submit([](photobridge::SqliteConnection& db) {
        return db.Execute("INSERT INTO queue_order(value) VALUES(1);");
    });
    EXPECT_TRUE(first.get().ok());
    EXPECT_TRUE(second.get().ok());
    queue.Stop();
    auto rejected = queue.Submit([](photobridge::SqliteConnection&) {
        return photobridge::Status::Ok();
    });
    EXPECT_EQ(rejected.get().code(), photobridge::StatusCode::kInvalidArgument);
    std::filesystem::remove(path, error);
}

TEST(DbCommandQueueTest, PropagatesCommandFailure)
{
    const auto path = std::filesystem::temp_directory_path()
        / "photobridge_db_command_queue_failure_test.db";
    std::error_code error;
    std::filesystem::remove(path, error);
    auto connection = photobridge::SqliteConnection::Open(path);
    ASSERT_TRUE(connection.ok());
    photobridge::DbCommandQueue queue(connection.value());
    auto result = queue.Submit([](photobridge::SqliteConnection& db) {
        return db.Execute("THIS IS NOT SQL;");
    });
    EXPECT_FALSE(result.get().ok());
    std::filesystem::remove(path, error);
}

TEST(DbCommandQueueTest, StopDrainsQueuedCommandsBeforeJoin)
{
    const auto path = std::filesystem::temp_directory_path()
        / "photobridge_db_command_queue_drain_test.db";
    std::error_code error;
    std::filesystem::remove(path, error);
    auto connection = photobridge::SqliteConnection::Open(path);
    ASSERT_TRUE(connection.ok());
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());
    photobridge::DbCommandQueue queue(connection.value());
    auto first = queue.Submit([](photobridge::SqliteConnection& db) {
        return db.Execute("CREATE TABLE drain_order(value INTEGER NOT NULL);");
    });
    auto second = queue.Submit([](photobridge::SqliteConnection& db) {
        return db.Execute("INSERT INTO drain_order(value) VALUES(1);");
    });
    queue.Stop();
    queue.Stop();
    EXPECT_TRUE(first.get().ok());
    EXPECT_TRUE(second.get().ok());
    std::filesystem::remove(path, error);
}

TEST(DbCommandQueueTest, AppliesBackpressureAtCapacity)
{
    const auto path = std::filesystem::temp_directory_path()
        / "photobridge_db_command_queue_capacity_test.db";
    std::error_code error;
    std::filesystem::remove(path, error);
    auto connection = photobridge::SqliteConnection::Open(path);
    ASSERT_TRUE(connection.ok());
    photobridge::DbCommandQueue queue(connection.value(), 1);
    auto started = std::make_shared<std::promise<void>>();
    auto release = std::make_shared<std::promise<void>>();
    auto release_future = std::make_shared<std::future<void>>(
        release->get_future());
    auto first = queue.Submit([
        started, release_future](photobridge::SqliteConnection&) {
            started->set_value();
            release_future->wait();
            return photobridge::Status::Ok();
        });
    started->get_future().wait();
    auto second = queue.Submit([](photobridge::SqliteConnection&) {
        return photobridge::Status::Ok();
    });
    auto third = queue.Submit([](photobridge::SqliteConnection&) {
        return photobridge::Status::Ok();
    });
    EXPECT_EQ(third.get().code(), photobridge::StatusCode::kIoError);
    release->set_value();
    EXPECT_TRUE(first.get().ok());
    EXPECT_TRUE(second.get().ok());
    std::filesystem::remove(path, error);
}

TEST(DbCommandQueueTest, ConvertsCommandExceptionToFailureAck)
{
    const auto path = std::filesystem::temp_directory_path()
        / "photobridge_db_command_queue_exception_test.db";
    std::error_code error;
    std::filesystem::remove(path, error);
    auto connection = photobridge::SqliteConnection::Open(path);
    ASSERT_TRUE(connection.ok());
    photobridge::DbCommandQueue queue(connection.value());
    auto result = queue.Submit([](photobridge::SqliteConnection&) -> photobridge::Status {
        throw std::runtime_error("fixture failure");
    });
    const auto status = result.get();
    EXPECT_EQ(status.code(), photobridge::StatusCode::kInternal);
    std::filesystem::remove(path, error);
}

TEST(DbCommandQueueTest, FixedBatchProvidesStableAckBenchmarkBaseline)
{
    const auto path = std::filesystem::temp_directory_path()
        / "photobridge_db_command_queue_benchmark_test.db";
    std::error_code error;
    std::filesystem::remove(path, error);
    auto connection = photobridge::SqliteConnection::Open(path);
    ASSERT_TRUE(connection.ok());
    photobridge::DbCommandQueue queue(connection.value(), 512);
    constexpr int kCommands = 256;
    std::vector<std::future<photobridge::Status>> acknowledgements;
    acknowledgements.reserve(kCommands);
    const auto started = std::chrono::steady_clock::now();
    for (int index = 0; index < kCommands; ++index) {
        acknowledgements.push_back(queue.Submit(
            [](photobridge::SqliteConnection& db) {
                return db.Execute("SELECT 1;");
            }));
    }
    for (auto& acknowledgement : acknowledgements) {
        ASSERT_TRUE(acknowledgement.get().ok());
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_GT(elapsed.count(), 0);
    queue.Stop();
    std::filesystem::remove(path, error);
}
