#include "photobridge/filesystem/linux_file_ops.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <linux/fs.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <sys/syscall.h>

#include "photobridge/common/posix_error.h"

namespace photobridge {
namespace {

Status ValidateLeafName(std::string_view name)
{
    if (name.empty()
        || name.find('/') != std::string_view::npos
        || name.find('\0') != std::string_view::npos
        || name == "."
        || name == "..") {
        return Status(
            StatusCode::kInvalidArgument,
            "name must be a single safe path component");
    }

    return Status::Ok();
}

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

    if (seconds > std::numeric_limits<std::int64_t>::max()
            / kNanosecondsPerSecond
        || seconds < std::numeric_limits<std::int64_t>::min()
            / kNanosecondsPerSecond) {
        return Status(
            StatusCode::kInternal,
            std::string("file timestamp overflows nanoseconds: ")
                + field_name);
    }

    if (seconds == kMax / kNanosecondsPerSecond
        && nanoseconds > kMax % kNanosecondsPerSecond) {
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

}  // namespace

StatusOr<UniqueFd> LinuxFileOps::OpenRoot(
    const std::filesystem::path& path,
    OpenRootMode mode)
{
    if (mode == OpenRootMode::kCreateIfMissing) {
        if (::mkdir(path.c_str(), 0755) == -1) {
            const int error_number = errno;
            if (error_number != EEXIST) {
                return StatusFromErrno(error_number, "mkdir workspace root");
            }
        }
    }

    const int fd = ::open(
        path.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd == -1) {
        const int error_number = errno;
        return StatusFromErrno(error_number, "open directory root");
    }

    return UniqueFd(fd);
}

StatusOr<UniqueFd> LinuxFileOps::OpenSource(
    int root_fd,
    const RelativePath& path)
{
    const int duplicate_fd = ::fcntl(
        root_fd,
        F_DUPFD_CLOEXEC,
        0);
    if (duplicate_fd == -1) {
        const int error_number = errno;
        return StatusFromErrno(error_number, "duplicate source root fd");
    }

    UniqueFd current_fd(duplicate_fd);
    const auto& components = path.components();

    for (std::size_t index = 0; index < components.size(); ++index) {
        const bool is_last = index + 1 == components.size();
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;
        if (!is_last) {
            flags |= O_DIRECTORY;
        }

        const int child_fd = ::openat(
            current_fd.get(),
            components[index].c_str(),
            flags);
        if (child_fd == -1) {
            const int error_number = errno;
            return StatusFromErrno(error_number, "open source relative path");
        }

        if (is_last) {
            return UniqueFd(child_fd);
        }

        current_fd.reset(child_fd);
    }

    return Status(
        StatusCode::kInvalidArgument,
        "relative path has no components");
}

StatusOr<FileIdentity> LinuxFileOps::StatFd(int fd)
{
    struct stat info {};
    if (::fstat(fd, &info) == -1) {
        const int error_number = errno;
        return StatusFromErrno(error_number, "fstat file descriptor");
    }

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

StatusOr<UniqueFd> LinuxFileOps::CreateTempNoReplace(
    int parent_fd,
    std::string_view temp_name,
    mode_t mode)
{
    const Status name_status = ValidateLeafName(temp_name);
    if (!name_status.ok()) {
        return name_status;
    }

    const std::string name(temp_name);
    const int fd = ::openat(
        parent_fd,
        name.c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        mode);
    if (fd == -1) {
        const int error_number = errno;
        return StatusFromErrno(
            error_number,
            "create temporary file");
    }

    return UniqueFd(fd);
}

StatusOr<std::size_t> LinuxFileOps::Read(
    int fd,
    std::span<std::byte> buffer)
{
    if (buffer.empty()) {
        return std::size_t{0};
    }

    if (buffer.size() > static_cast<std::size_t>(
            std::numeric_limits<ssize_t>::max())) {
        return Status(
            StatusCode::kInvalidArgument,
            "read buffer is too large");
    }

    while (true) {
        const ssize_t result = ::read(
            fd,
            buffer.data(),
            buffer.size());
        if (result >= 0) {
            return static_cast<std::size_t>(result);
        }

        const int error_number = errno;
        if (error_number == EINTR) {
            continue;
        }

        return StatusFromErrno(error_number, "read file descriptor");
    }
}

StatusOr<std::size_t> LinuxFileOps::Write(
    int fd,
    std::span<const std::byte> buffer)
{
    if (buffer.empty()) {
        return std::size_t{0};
    }

    if (buffer.size() > static_cast<std::size_t>(
            std::numeric_limits<ssize_t>::max())) {
        return Status(
            StatusCode::kInvalidArgument,
            "write buffer is too large");
    }

    while (true) {
        const ssize_t result = ::write(
            fd,
            buffer.data(),
            buffer.size());
        if (result >= 0) {
            return static_cast<std::size_t>(result);
        }

        const int error_number = errno;
        if (error_number == EINTR) {
            continue;
        }

        return StatusFromErrno(error_number, "write file descriptor");
    }
}

Status LinuxFileOps::Fdatasync(int fd)
{
    while (::fdatasync(fd) == -1) {
        const int error_number = errno;
        if (error_number == EINTR) {
            continue;
        }

        return StatusFromErrno(error_number, "fdatasync file descriptor");
    }

    return Status::Ok();
}

Status LinuxFileOps::FsyncDirectory(int dir_fd)
{
    while (::fsync(dir_fd) == -1) {
        const int error_number = errno;
        if (error_number == EINTR) {
            continue;
        }

        return StatusFromErrno(error_number, "fsync directory descriptor");
    }

    return Status::Ok();
}

Status LinuxFileOps::RenameNoReplace(
    int old_dir_fd,
    std::string_view old_name,
    int new_dir_fd,
    std::string_view new_name)
{
    const Status old_name_status = ValidateLeafName(old_name);
    if (!old_name_status.ok()) {
        return old_name_status;
    }

    const Status new_name_status = ValidateLeafName(new_name);
    if (!new_name_status.ok()) {
        return new_name_status;
    }

    const std::string old_name_string(old_name);
    const std::string new_name_string(new_name);
    const long result = ::syscall(
        SYS_renameat2,
        old_dir_fd,
        old_name_string.c_str(),
        new_dir_fd,
        new_name_string.c_str(),
        RENAME_NOREPLACE);
    if (result == -1) {
        const int error_number = errno;
        return StatusFromErrno(error_number, "rename without replace");
    }

    return Status::Ok();
}

Status LinuxFileOps::UnlinkAt(
    int dir_fd,
    std::string_view name)
{
    const Status name_status = ValidateLeafName(name);
    if (!name_status.ok()) {
        return name_status;
    }

    const std::string name_string(name);
    if (::unlinkat(dir_fd, name_string.c_str(), 0) == -1) {
        const int error_number = errno;
        return StatusFromErrno(error_number, "unlink directory entry");
    }

    return Status::Ok();
}

}  // namespace photobridge
