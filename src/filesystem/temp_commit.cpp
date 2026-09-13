#include "photobridge/filesystem/temp_commit.h"

#include <utility>

namespace photobridge {
namespace {

Status Invalid(const char* message)
{
    return Status(StatusCode::kInvalidArgument, message);
}

Status CleanupTemp(FileOps& file_ops, int parent_fd, const std::string& name)
{
    const Status status = file_ops.UnlinkAt(parent_fd, name);
    return status.ok()
        ? Status::Ok()
        : Status(
              StatusCode::kInternal,
              "failed to clean up temporary file after commit failure");
}

}  // namespace

StatusOr<TempCommitResult> CopyToTempAndPublish(
    FileOps& file_ops,
    Hasher& hasher,
    int source_fd,
    int target_parent_fd,
    std::string temp_name,
    std::string final_name,
    std::span<std::byte> buffer,
    mode_t mode)
{
    if (temp_name.empty() || final_name.empty()) {
        return Invalid("temporary and final names must not be empty");
    }
    if (temp_name == final_name) {
        return Invalid("temporary and final names must differ");
    }

    auto temp = file_ops.CreateTempNoReplace(
        target_parent_fd,
        temp_name,
        mode);
    if (!temp.ok()) {
        return temp.status();
    }

    auto copy = CopyAndHash(
        file_ops,
        hasher,
        source_fd,
        temp.value().get(),
        buffer);
    if (!copy.ok()) {
        static_cast<void>(CleanupTemp(file_ops, target_parent_fd, temp_name));
        return copy.status();
    }

    Status status = file_ops.Fdatasync(temp.value().get());
    if (!status.ok()) {
        static_cast<void>(CleanupTemp(file_ops, target_parent_fd, temp_name));
        return status;
    }

    status = file_ops.RenameNoReplace(
        target_parent_fd,
        temp_name,
        target_parent_fd,
        final_name);
    if (!status.ok()) {
        return status;
    }

    status = file_ops.FsyncDirectory(target_parent_fd);
    if (!status.ok()) {
        return status;
    }

    return TempCommitResult{
        std::move(copy.value()),
        std::move(temp_name),
        std::move(final_name),
    };
}

}  // namespace photobridge
