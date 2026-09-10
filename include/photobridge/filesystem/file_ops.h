#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>

#include <sys/types.h>

#include "photobridge/common/status_or.h"
#include "photobridge/common/unique_fd.h"
#include "photobridge/model/file_identity.h"
#include "photobridge/model/relative_path.h"

namespace photobridge {

enum class OpenRootMode {
    kExisting,
    kCreateIfMissing,
};

class FileOps {
public:
    virtual StatusOr<UniqueFd> OpenRoot(
        const std::filesystem::path& path,
        OpenRootMode mode) = 0;

    virtual StatusOr<UniqueFd> OpenSource(
        int root_fd,
        const RelativePath& path) = 0;

    virtual StatusOr<FileIdentity> StatFd(int fd) = 0;

    virtual StatusOr<UniqueFd> CreateTempNoReplace(
        int parent_fd,
        std::string_view temp_name,
        mode_t mode) = 0;

    virtual StatusOr<std::size_t> Read(
        int fd,
        std::span<std::byte> buffer) = 0;

    virtual StatusOr<std::size_t> Write(
        int fd,
        std::span<const std::byte> buffer) = 0;

    virtual Status Fdatasync(int fd) = 0;

    virtual Status FsyncDirectory(int dir_fd) = 0;

    virtual Status RenameNoReplace(
        int old_dir_fd,
        std::string_view old_name,
        int new_dir_fd,
        std::string_view new_name) = 0;

    virtual Status UnlinkAt(
        int dir_fd,
        std::string_view name) = 0;

    virtual ~FileOps() = default;
};

}  // namespace photobridge
