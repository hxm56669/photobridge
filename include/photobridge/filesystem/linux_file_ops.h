#pragma once

#include "photobridge/filesystem/file_ops.h"

namespace photobridge {

class LinuxFileOps final : public FileOps {
public:
    StatusOr<UniqueFd> OpenRoot(
        const std::filesystem::path& path,
        OpenRootMode mode) override;

    StatusOr<UniqueFd> OpenSource(
        int root_fd,
        const RelativePath& path) override;

    StatusOr<FileIdentity> StatFd(int fd) override;

    StatusOr<UniqueFd> CreateTempNoReplace(
        int parent_fd,
        std::string_view temp_name,
        mode_t mode) override;

    StatusOr<std::size_t> Read(
        int fd,
        std::span<std::byte> buffer) override;

    StatusOr<std::size_t> Write(
        int fd,
        std::span<const std::byte> buffer) override;

    Status Fdatasync(int fd) override;

    Status FsyncDirectory(int dir_fd) override;

    Status RenameNoReplace(
        int old_dir_fd,
        std::string_view old_name,
        int new_dir_fd,
        std::string_view new_name) override;

    Status UnlinkAt(
        int dir_fd,
        std::string_view name) override;
};

}  // namespace photobridge
