#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "photobridge/model/task.h"

namespace {

photobridge::RelativePath Path(const char* value)
{
    auto result = photobridge::RelativePath::Parse(value);
    EXPECT_TRUE(result.ok());
    return std::move(result.value());
}

photobridge::TaskSpec Task()
{
    return photobridge::TaskSpec{
        "task-id",
        "task-key",
        photobridge::TaskType::kMigrateFile,
        "logical-asset",
        "physical-asset",
        Path("album/photo.jpg"),
        42,
        std::nullopt,
    };
}

}  // namespace

TEST(TaskTest, TaskKeyIsStableAndBindsTaskSemantics)
{
    const auto first = photobridge::TaskKeyFor(Task());
    auto changed = Task();
    changed.target_path = Path("other/photo.jpg");
    const auto second = photobridge::TaskKeyFor(changed);

    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(first.value(), photobridge::TaskKeyFor(Task()).value());
    EXPECT_NE(first.value(), second.value());
    EXPECT_EQ(first.value().size(), 69U);
}

TEST(TaskTest, TaskKeyExcludesExecutionAndOutputMetadata)
{
    auto base = Task();
    auto changed = base;
    changed.id = "another-id";
    changed.task_key = "another-key";
    changed.estimated_bytes = 999;
    changed.expected_digest = photobridge::Digest{};

    const auto first = photobridge::TaskKeyFor(base);
    const auto second = photobridge::TaskKeyFor(changed);
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(first.value(), second.value());
}

TEST(TaskTest, ValidatesImmutableAndRuntimeFields)
{
    auto invalid = Task();
    invalid.source_asset_id.reset();
    EXPECT_FALSE(photobridge::ValidateTaskSpec(invalid).ok());

    photobridge::TaskRuntime runtime;
    runtime.id = "task-id";
    runtime.state = photobridge::TaskState::kRunning;
    EXPECT_FALSE(photobridge::ValidateTaskRuntime(runtime).ok());
    runtime.owner_epoch.value = 3;
    runtime.attempt_id = "attempt-1";
    EXPECT_TRUE(photobridge::ValidateTaskRuntime(runtime).ok());
}

TEST(TaskTest, CentralizesTaskAndFileAttemptTransitions)
{
    using photobridge::FileAttemptState;
    using photobridge::TaskState;

    EXPECT_TRUE(
        photobridge::IsValidTransition(TaskState::kPlanned, TaskState::kReady));
    EXPECT_TRUE(
        photobridge::IsValidTransition(TaskState::kRunning, TaskState::kSucceeded));
    EXPECT_TRUE(
        photobridge::IsValidTransition(
            FileAttemptState::kRunning,
            FileAttemptState::kCommitIntent));
    EXPECT_TRUE(
        photobridge::IsValidTransition(
            FileAttemptState::kVerifiedDurable,
            FileAttemptState::kCommitted));
    EXPECT_FALSE(
        photobridge::IsValidTransition(
            TaskState::kPlanned,
            TaskState::kSucceeded));
    EXPECT_FALSE(
        photobridge::IsValidTransition(
            FileAttemptState::kCommitted,
            FileAttemptState::kRunning));
}

TEST(TaskTest, ExposesStableTaskTypeContract)
{
    EXPECT_STREQ(
        photobridge::TaskTypeName(photobridge::TaskType::kMigrateFile),
        "MIGRATE_FILE");
    EXPECT_TRUE(photobridge::IsKnownTaskType(
        photobridge::TaskType::kWriteReport));
    EXPECT_FALSE(photobridge::IsKnownTaskType(
        static_cast<photobridge::TaskType>(99)));
    auto invalid = Task();
    invalid.type = static_cast<photobridge::TaskType>(99);
    EXPECT_EQ(
        photobridge::ValidateTaskSpec(invalid).code(),
        photobridge::StatusCode::kInvalidArgument);
}

TEST(TaskTest, PublishesTaskKeySemanticVersion)
{
    EXPECT_EQ(
        photobridge::kTaskKeySemanticVersion,
        std::string_view("PB_TASK_V1"));
    EXPECT_EQ(
        photobridge::TaskKeyFor(Task()).value(),
        photobridge::TaskKeyFor(Task()).value());
}
