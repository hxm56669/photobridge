#include "photobridge/filesystem/linux_directory_walker.h"

#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

#include "photobridge/common/posix_error.h"
#include "photobridge/common/unique_fd.h"

namespace photobridge {
namespace {

StatusOr<std::int64_t> TimespecToNanoseconds(
    const timespec& timestamp,
    const char* field_name)
{
    constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
    const auto seconds = static_cast<std::int64_t>(timestamp.tv_sec);
    const auto nanoseconds = static_cast<std::int64_t>(timestamp.tv_nsec);

    if (nanoseconds < 0 || nanoseconds >= kNanosecondsPerSecond) {
        return Status(
            StatusCode::kInternal,
            std::string("invalid file timestamp: ") + field_name);
    }

    constexpr std::int64_t kMax =
        std::numeric_limits<std::int64_t>::max();
    if (seconds > kMax / kNanosecondsPerSecond
        || seconds < std::numeric_limits<std::int64_t>::min()
            / kNanosecondsPerSecond
        || (seconds == kMax / kNanosecondsPerSecond
            && nanoseconds > kMax % kNanosecondsPerSecond)) {
        return Status(
            StatusCode::kInternal,
            std::string("file timestamp overflows nanoseconds: ")
                + field_name);
    }

    return seconds * kNanosecondsPerSecond + nanoseconds;
}

template <typename Integer>
StatusOr<std::uint64_t> ToUint64(
    Integer value,
    const char* field_name)
{
    static_assert(std::is_integral_v<Integer>);

    using UnsignedInteger = std::make_unsigned_t<Integer>;
    if constexpr (std::is_signed_v<Integer>) {
        if (value < 0) {
            return Status(
                StatusCode::kInternal,
                std::string("negative file identity field: ")
                    + field_name);
        }
    }

    const auto unsigned_value = static_cast<UnsignedInteger>(value);
    if constexpr (
        std::numeric_limits<UnsignedInteger>::digits
        > std::numeric_limits<std::uint64_t>::digits) {
        if (unsigned_value
            > static_cast<UnsignedInteger>(
                std::numeric_limits<std::uint64_t>::max())) {
            return Status(
                StatusCode::kInternal,
                std::string("file identity field overflows uint64: ")
                    + field_name);
        }
    }

    return static_cast<std::uint64_t>(unsigned_value);
}

StatusOr<FileIdentity> FileIdentityFromStat(const struct stat& info)
{
    auto mtime_ns = TimespecToNanoseconds(info.st_mtim, "mtime");
    if (!mtime_ns.ok()) {
        return mtime_ns.status();
    }

    auto ctime_ns = TimespecToNanoseconds(info.st_ctim, "ctime");
    if (!ctime_ns.ok()) {
        return ctime_ns.status();
    }

    auto device = ToUint64(info.st_dev, "device");
    if (!device.ok()) {
        return device.status();
    }

    auto inode = ToUint64(info.st_ino, "inode");
    if (!inode.ok()) {
        return inode.status();
    }

    auto size = ToUint64(info.st_size, "size");
    if (!size.ok()) {
        return size.status();
    }

    FileIdentity identity;
    identity.device = device.value();
    identity.inode = inode.value();
    identity.size = size.value();
    identity.mtime_ns = mtime_ns.value();
    identity.ctime_ns = ctime_ns.value();
    return identity;
}

DirectoryEntryKind KindFromMode(mode_t mode)
{
    if (S_ISREG(mode)) {
        return DirectoryEntryKind::kRegularFile;
    }
    if (S_ISDIR(mode)) {
        return DirectoryEntryKind::kDirectory;
    }
    if (S_ISLNK(mode)) {
        return DirectoryEntryKind::kSymlink;
    }
    return DirectoryEntryKind::kOther;
}

std::string JoinPath(
    const std::vector<std::string>& parent_components,
    std::string_view name)
{
    std::string result;
    for (const auto& component : parent_components) {
        if (!result.empty()) {
            result.push_back('/');
        }
        result += component;
    }
    if (!result.empty()) {
        result.push_back('/');
    }
    result.append(name.data(), name.size());
    return result;
}

Status WalkDirectory(
    int directory_fd,
    std::vector<std::string>& parent_components,
    DirectoryEntrySink& sink)
{
    const int duplicate_fd = ::fcntl(
        directory_fd,
        F_DUPFD_CLOEXEC,
        0);
    if (duplicate_fd == -1) {
        const int error_number = errno;
        return StatusFromErrno(error_number, "duplicate directory fd");
    }

    UniqueFd owned_fd(duplicate_fd);
    DIR* raw_directory = ::fdopendir(owned_fd.get());
    if (raw_directory == nullptr) {
        const int error_number = errno;
        return StatusFromErrno(error_number, "open directory stream");
    }
    owned_fd.release();
    std::unique_ptr<DIR, int (*)(DIR*)> directory(
        raw_directory,
        &::closedir);

    std::vector<std::string> names;
    while (true) {
        errno = 0;
        const dirent* entry = ::readdir(directory.get());
        if (entry == nullptr) {
            const int error_number = errno;
            if (error_number != 0) {
                return StatusFromErrno(
                    error_number,
                    "read directory entry");
            }
            break;
        }

        const std::string_view name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }

        names.emplace_back(name);
    }

    std::sort(names.begin(), names.end());

    for (const std::string& name_string : names) {
        const std::string_view name(name_string);

        struct stat info {
        };
        if (::fstatat(
                directory_fd,
                name_string.c_str(),
                &info,
                AT_SYMLINK_NOFOLLOW)
            == -1) {
            const int error_number = errno;
            return StatusFromErrno(
                error_number,
                "stat directory entry");
        }

        auto identity = FileIdentityFromStat(info);
        if (!identity.ok()) {
            return identity.status();
        }

        auto relative_path = RelativePath::Parse(
            JoinPath(parent_components, name));
        if (!relative_path.ok()) {
            return relative_path.status();
        }

        const DirectoryEntryKind kind = KindFromMode(info.st_mode);
        Status add_status = sink.Add(DirectoryEntry{
            std::move(relative_path.value()),
            identity.value(),
            kind,
        });
        if (!add_status.ok()) {
            return add_status;
        }

        if (kind != DirectoryEntryKind::kDirectory) {
            continue;
        }

        const int child_fd = ::openat(
            directory_fd,
            name_string.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (child_fd == -1) {
            const int error_number = errno;
            return StatusFromErrno(
                error_number,
                "open child directory");
        }

        UniqueFd child_directory(child_fd);
        parent_components.push_back(name_string);
        const Status child_status = WalkDirectory(
            child_directory.get(),
            parent_components,
            sink);
        parent_components.pop_back();
        if (!child_status.ok()) {
            return child_status;
        }
    }

    return Status::Ok();
}

}  // namespace

Status LinuxDirectoryWalker::Walk(
    int root_fd,
    DirectoryEntrySink& sink)
{
    std::vector<std::string> parent_components;
    return WalkDirectory(root_fd, parent_components, sink);
}

}  // namespace photobridge
