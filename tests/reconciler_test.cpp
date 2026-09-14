#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "photobridge/filesystem/reconciler.h"

namespace {

photobridge::RelativePath Path(const char* value)
{
    auto result = photobridge::RelativePath::Parse(value);
    EXPECT_TRUE(result.ok());
    return std::move(result.value());
}

photobridge::TaskSpec Spec()
{
    return photobridge::TaskSpec{
        "task-1",
        "key-1",
        photobridge::TaskType::kMigrateFile,
        "asset-1",
        "source-1",
        Path("photo.jpg"),
        10,
        std::nullopt,
    };
}

photobridge::TaskRuntime Runtime(photobridge::TaskState state)
{
    photobridge::TaskRuntime runtime;
    runtime.id = "task-1";
    runtime.state = state;
    runtime.owner_epoch.value = 2;
    runtime.attempt_id = "attempt-1";
    return runtime;
}

photobridge::CommitIntent Intent()
{
    return photobridge::CommitIntent{
        "task-1",
        "attempt-1",
        {2},
        ".photo.pbtmp",
        "photo.jpg",
    };
}

photobridge::VerifiedReceipt Receipt()
{
    photobridge::Digest digest;
    digest.bytes[0] = std::byte{0x42};
    return photobridge::VerifiedReceipt{
        "task-1",
        "attempt-1",
        {2},
        ".photo.pbtmp",
        "photo.jpg",
        10,
        std::nullopt,
        digest,
        std::nullopt,
    };
}

photobridge::ObservedFileState MatchingFinal(bool temp)
{
    photobridge::ObservedFileState observed;
    observed.temp_exists = temp;
    observed.final_exists = true;
    observed.temp_size = 10;
    observed.final_size = 10;
    observed.final_digest = Receipt().target_digest;
    if (temp) observed.temp_digest = Receipt().target_digest;
    return observed;
}

}  // namespace

TEST(ReconcilerTest, RedoesIntentWithoutReceiptAndRejectsUnprovenFinal)
{
    const auto spec = Spec();
    const auto runtime = Runtime(photobridge::TaskState::kRunning);
    const auto intent = Intent();

    photobridge::ObservedFileState absent;
    auto retry = photobridge::Decide(spec, runtime, intent, std::nullopt, absent);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(
        retry.value().action,
        photobridge::ReconcileAction::kRetryTask);

    photobridge::ObservedFileState unproven_temp;
    unproven_temp.temp_exists = true;
    auto retry_with_temp = photobridge::Decide(
        spec,
        runtime,
        intent,
        std::nullopt,
        unproven_temp);
    ASSERT_TRUE(retry_with_temp.ok());
    EXPECT_EQ(
        retry_with_temp.value().action,
        photobridge::ReconcileAction::kRetryTask);
    EXPECT_NE(
        retry_with_temp.value().reason.find("without cleaning"),
        std::string::npos);

    photobridge::ObservedFileState final_only;
    final_only.final_exists = true;
    auto conflict = photobridge::Decide(
        spec,
        runtime,
        intent,
        std::nullopt,
        final_only);
    ASSERT_TRUE(conflict.ok());
    EXPECT_EQ(
        conflict.value().action,
        photobridge::ReconcileAction::kTargetConflict);
}

TEST(ReconcilerTest, ResumesMatchingTempOrAdoptsMatchingFinal)
{
    const auto spec = Spec();
    const auto runtime = Runtime(photobridge::TaskState::kRunning);
    const auto intent = Intent();
    const auto receipt = Receipt();

    photobridge::ObservedFileState temp_only;
    temp_only.temp_exists = true;
    temp_only.temp_size = 10;
    temp_only.temp_digest = receipt.target_digest;
    auto resume = photobridge::Decide(
        spec,
        runtime,
        intent,
        receipt,
        temp_only);
    ASSERT_TRUE(resume.ok());
    EXPECT_EQ(
        resume.value().action,
        photobridge::ReconcileAction::kResumeCommitFromTemp);

    auto adopt = photobridge::Decide(
        spec,
        runtime,
        intent,
        receipt,
        MatchingFinal(false));
    ASSERT_TRUE(adopt.ok());
    EXPECT_EQ(
        adopt.value().action,
        photobridge::ReconcileAction::kAdoptFinal);

    auto adopt_and_clean = photobridge::Decide(
        spec,
        runtime,
        intent,
        receipt,
        MatchingFinal(true));
    ASSERT_TRUE(adopt_and_clean.ok());
    EXPECT_EQ(
        adopt_and_clean.value().action,
        photobridge::ReconcileAction::kAdoptFinalAndCleanupTemp);

    auto mismatched_temp = MatchingFinal(true);
    mismatched_temp.temp_digest = std::nullopt;
    auto adopt_only = photobridge::Decide(
        spec,
        runtime,
        intent,
        receipt,
        mismatched_temp);
    ASSERT_TRUE(adopt_only.ok());
    EXPECT_EQ(
        adopt_only.value().action,
        photobridge::ReconcileAction::kAdoptFinal);
}

TEST(ReconcilerTest, DistinguishesCommittedMismatchAndUnavailableSource)
{
    const auto spec = Spec();
    const auto intent = Intent();
    const auto receipt = Receipt();

    auto committed = photobridge::Decide(
        spec,
        Runtime(photobridge::TaskState::kSucceeded),
        intent,
        receipt,
        MatchingFinal(false));
    ASSERT_TRUE(committed.ok());
    EXPECT_EQ(
        committed.value().action,
        photobridge::ReconcileAction::kReverifyCommitted);

    photobridge::ObservedFileState absent;
    auto inconsistent = photobridge::Decide(
        spec,
        Runtime(photobridge::TaskState::kRunning),
        intent,
        receipt,
        absent);
    ASSERT_TRUE(inconsistent.ok());
    EXPECT_EQ(
        inconsistent.value().action,
        photobridge::ReconcileAction::kInconsistent);
}

TEST(ReconcilerTest, SourceReplacementIsNotRetryable)
{
    auto observed = photobridge::ObservedFileState{};
    observed.source_changed = true;
    auto result = photobridge::Decide(
        Spec(),
        Runtime(photobridge::TaskState::kRunning),
        Intent(),
        Receipt(),
        observed);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(
        result.value().action,
        photobridge::ReconcileAction::kInconsistent);
    EXPECT_NE(result.value().reason.find("SOURCE_CHANGED"), std::string::npos);
}

TEST(ReconcilerTest, RejectsIntentBindingMismatch)
{
    auto intent = Intent();
    intent.final_path = "other.jpg";
    photobridge::ObservedFileState observed;
    const auto result = photobridge::Decide(
        Spec(),
        Runtime(photobridge::TaskState::kRunning),
        intent,
        std::nullopt,
        observed);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), photobridge::StatusCode::kInvalidArgument);
}
