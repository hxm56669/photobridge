#include <filesystem>
#include <fstream>
#include <cstdint>
#include <array>
#include <string>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include "photobridge/filesystem/linux_file_ops.h"

namespace {

class LinuxFileOpsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path()
            / "photobridge_linux_file_ops_test";

        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path root_;
};

}  // namespace

TEST_F(LinuxFileOpsTest, OpensExistingDirectoryAsOwnedFd)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(root_, error));
    ASSERT_FALSE(error);

    photobridge::LinuxFileOps file_ops;
    auto result = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);

    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.value());

    struct stat info {
    };
    ASSERT_EQ(::fstat(result.value().get(), &info), 0);
    EXPECT_TRUE(S_ISDIR(info.st_mode));
}

TEST_F(LinuxFileOpsTest, CreatesMissingDirectoryWhenRequested)
{
    photobridge::LinuxFileOps file_ops;
    auto result = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kCreateIfMissing);

    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(std::filesystem::is_directory(root_));
}

TEST_F(LinuxFileOpsTest, MissingDirectoryReturnsNotFound)
{
    photobridge::LinuxFileOps file_ops;
    auto result = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(
        result.status().code(),
        photobridge::StatusCode::kNotFound);
}

TEST_F(LinuxFileOpsTest, OpensSourceRelativeToRootFd)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(
        root_ / "album",
        error));
    ASSERT_FALSE(error);

    {
        std::ofstream file(root_ / "album" / "photo.jpg");
        ASSERT_TRUE(file);
        file << "photo data";
    }

    photobridge::LinuxFileOps file_ops;
    auto root = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root.ok());

    const auto relative_path = photobridge::RelativePath::Parse(
        "album/photo.jpg");
    ASSERT_TRUE(relative_path.ok());

    auto source = file_ops.OpenSource(
        root.value().get(),
        relative_path.value());
    ASSERT_TRUE(source.ok());

    char buffer[16] = {};
    ASSERT_EQ(::read(source.value().get(), buffer, sizeof(buffer)), 10);
    EXPECT_EQ(std::string(buffer, 10), "photo data");
}

TEST_F(LinuxFileOpsTest, RejectsSymlinkedFinalSourcePath)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(root_, error));
    ASSERT_FALSE(error);

    {
        std::ofstream file(root_ / "photo.jpg");
        ASSERT_TRUE(file);
        file << "photo data";
    }
    std::filesystem::create_symlink(
        root_ / "photo.jpg",
        root_ / "link.jpg",
        error);
    ASSERT_FALSE(error);

    photobridge::LinuxFileOps file_ops;
    auto root = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root.ok());

    const auto relative_path = photobridge::RelativePath::Parse(
        "link.jpg");
    ASSERT_TRUE(relative_path.ok());

    const auto source = file_ops.OpenSource(
        root.value().get(),
        relative_path.value());
    EXPECT_FALSE(source.ok());
}

TEST_F(LinuxFileOpsTest, RejectsSymlinkedIntermediateDirectory)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(
        root_ / "real_album",
        error));
    ASSERT_FALSE(error);

    {
        std::ofstream file(root_ / "real_album" / "photo.jpg");
        ASSERT_TRUE(file);
        file << "photo data";
    }
    std::filesystem::create_directory_symlink(
        root_ / "real_album",
        root_ / "album",
        error);
    ASSERT_FALSE(error);

    photobridge::LinuxFileOps file_ops;
    auto root = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root.ok());

    const auto relative_path = photobridge::RelativePath::Parse(
        "album/photo.jpg");
    ASSERT_TRUE(relative_path.ok());

    const auto source = file_ops.OpenSource(
        root.value().get(),
        relative_path.value());
    EXPECT_FALSE(source.ok());
}

TEST_F(LinuxFileOpsTest, StatsOpenedFdIntoFileIdentity)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(root_, error));
    ASSERT_FALSE(error);

    {
        std::ofstream file(root_ / "photo.jpg");
        ASSERT_TRUE(file);
        file << "photo data";
    }

    const int raw_fd = ::open(
        (root_ / "photo.jpg").c_str(),
        O_RDONLY | O_CLOEXEC);
    ASSERT_GE(raw_fd, 0);
    photobridge::UniqueFd fd(raw_fd);

    photobridge::LinuxFileOps file_ops;
    auto result = file_ops.StatFd(fd.get());

    ASSERT_TRUE(result.ok());

    struct stat info {
    };
    ASSERT_EQ(::fstat(fd.get(), &info), 0);

    EXPECT_EQ(
        result.value().device,
        static_cast<std::uint64_t>(info.st_dev));
    EXPECT_EQ(
        result.value().inode,
        static_cast<std::uint64_t>(info.st_ino));
    EXPECT_EQ(result.value().size, 10U);
    EXPECT_EQ(
        result.value().mtime_ns,
        static_cast<std::int64_t>(info.st_mtim.tv_sec)
            * 1'000'000'000
            + static_cast<std::int64_t>(info.st_mtim.tv_nsec));
    EXPECT_EQ(
        result.value().ctime_ns,
        static_cast<std::int64_t>(info.st_ctim.tv_sec)
            * 1'000'000'000
            + static_cast<std::int64_t>(info.st_ctim.tv_nsec));
    EXPECT_FALSE(result.value().mount_id.has_value());
}

TEST_F(LinuxFileOpsTest, StatsOpenedFdAfterPathIsReplaced)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(root_, error));
    ASSERT_FALSE(error);

    {
        std::ofstream file(root_ / "photo.jpg");
        ASSERT_TRUE(file);
        file << "original photo";
    }

    const int raw_fd = ::open(
        (root_ / "photo.jpg").c_str(),
        O_RDONLY | O_CLOEXEC);
    ASSERT_GE(raw_fd, 0);
    photobridge::UniqueFd fd(raw_fd);

    photobridge::LinuxFileOps file_ops;
    const auto original = file_ops.StatFd(fd.get());
    ASSERT_TRUE(original.ok());

    std::filesystem::rename(
        root_ / "photo.jpg",
        root_ / "photo.old",
        error);
    ASSERT_FALSE(error);

    {
        std::ofstream file(root_ / "photo.jpg");
        ASSERT_TRUE(file);
        file << "new";
    }

    const auto still_original = file_ops.StatFd(fd.get());
    ASSERT_TRUE(still_original.ok());
    EXPECT_EQ(still_original.value().device, original.value().device);
    EXPECT_EQ(still_original.value().inode, original.value().inode);
    EXPECT_EQ(still_original.value().size, original.value().size);
    EXPECT_EQ(still_original.value().mtime_ns, original.value().mtime_ns);
    EXPECT_GE(still_original.value().ctime_ns, original.value().ctime_ns);
    EXPECT_EQ(std::filesystem::file_size(root_ / "photo.jpg"), 3U);
}

TEST_F(LinuxFileOpsTest, InvalidFdReturnsInternalStatus)
{
    photobridge::LinuxFileOps file_ops;
    const auto result = file_ops.StatFd(-1);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(
        result.status().code(),
        photobridge::StatusCode::kInternal);
}

TEST_F(LinuxFileOpsTest, CreatesTempFileWithoutReplacingExistingFile)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(root_, error));
    ASSERT_FALSE(error);

    photobridge::LinuxFileOps file_ops;
    auto root = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root.ok());

    const std::string temp_name = ".photobridge.test.pbtmp";
    auto first = file_ops.CreateTempNoReplace(
        root.value().get(),
        temp_name,
        0600);
    ASSERT_TRUE(first.ok());

    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / temp_name));

    auto second = file_ops.CreateTempNoReplace(
        root.value().get(),
        temp_name,
        0600);
    ASSERT_FALSE(second.ok());
    EXPECT_EQ(
        second.status().code(),
        photobridge::StatusCode::kAlreadyExists);
}

TEST_F(LinuxFileOpsTest, RejectsTempNameThatEscapesParentDirectory)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(root_, error));
    ASSERT_FALSE(error);

    photobridge::LinuxFileOps file_ops;
    auto root = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root.ok());

    const auto result = file_ops.CreateTempNoReplace(
        root.value().get(),
        "nested/temp.pbtmp",
        0600);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(
        result.status().code(),
        photobridge::StatusCode::kInvalidArgument);
}

TEST_F(LinuxFileOpsTest, WritesAndReadsBytesWithShortOperationSemantics)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(root_, error));
    ASSERT_FALSE(error);

    photobridge::LinuxFileOps file_ops;
    auto root = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root.ok());

    auto temp = file_ops.CreateTempNoReplace(
        root.value().get(),
        ".photobridge.io.pbtmp",
        0600);
    ASSERT_TRUE(temp.ok());

    const std::string payload = "photo data";
    const auto write_buffer = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(payload.data()),
        payload.size());
    const auto written = file_ops.Write(
        temp.value().get(),
        write_buffer);
    ASSERT_TRUE(written.ok());
    EXPECT_EQ(written.value(), payload.size());

    ASSERT_EQ(::lseek(temp.value().get(), 0, SEEK_SET), 0);

    std::array<std::byte, 32> read_storage{};
    const auto read_buffer = std::span<std::byte>(read_storage);
    const auto read = file_ops.Read(
        temp.value().get(),
        read_buffer);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value(), payload.size());

    const std::string actual(
        reinterpret_cast<const char*>(read_storage.data()),
        read.value());
    EXPECT_EQ(actual, payload);
}

TEST_F(LinuxFileOpsTest, InvalidFdReturnsIoStatusForReadAndWrite)
{
    photobridge::LinuxFileOps file_ops;
    std::array<std::byte, 1> storage{};
    const auto read = file_ops.Read(-1, std::span<std::byte>(storage));
    const auto write = file_ops.Write(
        -1,
        std::span<const std::byte>(storage));

    ASSERT_FALSE(read.ok());
    ASSERT_FALSE(write.ok());
    EXPECT_EQ(read.status().code(), photobridge::StatusCode::kInternal);
    EXPECT_EQ(write.status().code(), photobridge::StatusCode::kInternal);
}

TEST_F(LinuxFileOpsTest, SynchronizesFileAndDirectoryDescriptors)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(root_, error));
    ASSERT_FALSE(error);

    photobridge::LinuxFileOps file_ops;
    auto root = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root.ok());

    auto temp = file_ops.CreateTempNoReplace(
        root.value().get(),
        ".photobridge.sync.pbtmp",
        0600);
    ASSERT_TRUE(temp.ok());

    EXPECT_TRUE(file_ops.Fdatasync(temp.value().get()).ok());
    EXPECT_TRUE(file_ops.FsyncDirectory(root.value().get()).ok());
}

TEST_F(LinuxFileOpsTest, InvalidDescriptorFailsSynchronization)
{
    photobridge::LinuxFileOps file_ops;

    const auto file_status = file_ops.Fdatasync(-1);
    const auto directory_status = file_ops.FsyncDirectory(-1);

    EXPECT_FALSE(file_status.ok());
    EXPECT_FALSE(directory_status.ok());
    EXPECT_EQ(file_status.code(), photobridge::StatusCode::kInternal);
    EXPECT_EQ(
        directory_status.code(),
        photobridge::StatusCode::kInternal);
}

TEST_F(LinuxFileOpsTest, RenamesWithoutReplacingExistingTarget)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(root_, error));
    ASSERT_FALSE(error);

    photobridge::LinuxFileOps file_ops;
    auto root = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root.ok());

    auto source = file_ops.CreateTempNoReplace(
        root.value().get(),
        ".photobridge.source.pbtmp",
        0600);
    ASSERT_TRUE(source.ok());
    auto target = file_ops.CreateTempNoReplace(
        root.value().get(),
        "final.jpg",
        0600);
    ASSERT_TRUE(target.ok());
    target.value().reset();

    const auto conflict = file_ops.RenameNoReplace(
        root.value().get(),
        ".photobridge.source.pbtmp",
        root.value().get(),
        "final.jpg");
    EXPECT_FALSE(conflict.ok());
    EXPECT_EQ(
        conflict.code(),
        photobridge::StatusCode::kAlreadyExists);

    const auto published = file_ops.RenameNoReplace(
        root.value().get(),
        ".photobridge.source.pbtmp",
        root.value().get(),
        "published.jpg");
    EXPECT_TRUE(published.ok());
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "published.jpg"));
}

TEST_F(LinuxFileOpsTest, UnlinkAtRemovesOnlyNamedEntry)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(root_, error));
    ASSERT_FALSE(error);

    photobridge::LinuxFileOps file_ops;
    auto root = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root.ok());

    auto temp = file_ops.CreateTempNoReplace(
        root.value().get(),
        ".photobridge.delete.pbtmp",
        0600);
    ASSERT_TRUE(temp.ok());
    temp.value().reset();

    EXPECT_TRUE(file_ops.UnlinkAt(
        root.value().get(),
        ".photobridge.delete.pbtmp").ok());
    EXPECT_FALSE(std::filesystem::exists(
        root_ / ".photobridge.delete.pbtmp"));
}
