#include "photobridge/app/migration_attempt_preparer.h"

#include <optional>

#include "photobridge/common/test_hooks.h"
#include "photobridge/filesystem/binary_verifier.h"
#include "photobridge/filesystem/copy_and_hash.h"
#include "photobridge/filesystem/io_uring_copy_engine.h"
#include "photobridge/filesystem/linux_file_ops.h"

namespace photobridge {
namespace {

Status Invalid(const char* message)
{
    return Status(StatusCode::kInvalidArgument, message);
}

}  // namespace

StatusOr<VerifiedReceipt> MigrationAttemptPreparer::Prepare(
    FileOps& file_ops,
    MigrationRuntimeStore& repository,
    MutationGuard& source_guard,
    int target_root_fd,
    const MinimalPlanAsset& plan_asset,
    const std::string& plan_id,
    const std::string& task_id,
    ExecutionEpoch execution_epoch,
    const std::string& attempt_id,
    const std::string& temp_name,
    std::span<std::byte> buffer)
{
    if (plan_id.empty() || task_id.empty() || attempt_id.empty()
        || temp_name.empty()) {
        return Invalid("migration attempt fields must not be empty");
    }
    if (plan_asset.target_path.components().size() != 1) {
        return Invalid(
            "single-thread temp preparation requires a single target path component");
    }

    Status status = repository.MarkCommitIntent(
        plan_id, task_id, execution_epoch, attempt_id);
    if (!status.ok()) return status;

    auto temp_fd = file_ops.CreateTempNoReplace(target_root_fd, temp_name, 0600);
    if (!temp_fd.ok()) return temp_fd.status();

    Blake3Hasher hasher;
    status = source_guard.VerifyBeforeRead();
    if (!status.ok()) return status;
    auto accelerated = TryIoUringCopyAndHash(
        hasher,
        source_guard.fd(),
        temp_fd.value().get(),
        source_guard.manifest_identity().size);
    if (!accelerated.ok()) return accelerated.status();
    CopyResult copy;
    if (accelerated.value().has_value()) {
        copy = accelerated.value().value();
        status = source_guard.VerifyAfterRead();
        if (!status.ok()) return status;
    } else {
        auto synchronous = CopyAndHash(
            file_ops, hasher, source_guard, temp_fd.value().get(), buffer);
        if (!synchronous.ok()) return synchronous.status();
        copy = synchronous.value();
    }

    status = file_ops.Fdatasync(temp_fd.value().get());
    if (!status.ok()) return status;
    status = repository.MarkTempWritten(
        plan_id, task_id, execution_epoch, attempt_id);
    if (!status.ok()) return status;
    PauseForTest("PHOTOBRIDGE_TEST_PAUSE_BEFORE_RECEIPT_MS");

    auto temp_path = RelativePath::Parse(temp_name);
    if (!temp_path.ok()) return temp_path.status();
    auto verify_fd = file_ops.OpenSource(target_root_fd, temp_path.value());
    if (!verify_fd.ok()) return verify_fd.status();

    Blake3Hasher target_hasher;
    auto target_verification = VerifyBinary(
        file_ops,
        target_hasher,
        verify_fd.value().get(),
        copy.bytes_copied,
        std::optional<Digest>(copy.source_digest),
        buffer);
    if (!target_verification.ok()) return target_verification.status();
    if (target_verification.value().status != BinaryVerification::kIdentical) {
        return Status(
            StatusCode::kInternal,
            "temporary file failed independent binary verification");
    }

    const VerifiedReceipt receipt{
        task_id,
        attempt_id,
        execution_epoch,
        temp_name,
        std::string(plan_asset.target_path.bytes()),
        copy.bytes_copied,
        copy.source_digest,
        target_verification.value().target_digest,
        source_guard.manifest_identity(),
    };
    status = repository.PersistVerifiedReceipt(plan_id, receipt);
    if (!status.ok()) return status;
    PauseForTest("PHOTOBRIDGE_TEST_PAUSE_AFTER_RECEIPT_MS");
    return receipt;
}

}  // namespace photobridge
