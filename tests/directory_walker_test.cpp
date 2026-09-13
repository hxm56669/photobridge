#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "photobridge/filesystem/linux_directory_walker.h"
#include "photobridge/filesystem/linux_file_ops.h"

namespace {

class CollectingSink final : public photobridge::DirectoryEntrySink {
public:
    photobridge::Status Add(photobridge::DirectoryEntry entry) override
    {
        entries.emplace(
            std::string(entry.relative_path.bytes()),
            entry.kind);
        return photobridge::Status::Ok();
    }

    std::map<std::string, photobridge::DirectoryEntryKind> entries;
};

class FailingSink final : public photobridge::DirectoryEntrySink {
public:
    photobridge::Status Add(photobridge::DirectoryEntry) override
    {
        ++calls;
        return photobridge::Status(
            photobridge::StatusCode::kInternal,
            "sink stopped scan");
    }

    int calls = 0;
};

class RemovingDirectorySink final : public photobridge::DirectoryEntrySink {
public:
    explicit RemovingDirectorySink(std::filesystem::path root)
        : root_(std::move(root)) {}

    photobridge::Status Add(photobridge::DirectoryEntry entry) override
    {
        if (entry.kind != photobridge::DirectoryEntryKind::kDirectory) {
            return photobridge::Status::Ok();
        }

        std::error_code error;
        std::filesystem::remove_all(
            root_ / std::string(entry.relative_path.bytes()),
            error);
        if (error) {
            return photobridge::Status(
                photobridge::StatusCode::kIoError,
                error.message());
        }
        return photobridge::Status::Ok();
    }

private:
    std::filesystem::path root_;
};

class DirectoryWalkerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path()
            / "photobridge_directory_walker_test";
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

TEST_F(DirectoryWalkerTest, WalksNestedEntriesAndDoesNotFollowSymlinks)
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
    {
        std::ofstream file(root_ / "notes.txt");
        ASSERT_TRUE(file);
        file << "notes";
    }
    std::filesystem::create_symlink(
        root_ / "album" / "photo.jpg",
        root_ / "link.jpg",
        error);
    ASSERT_FALSE(error);

    photobridge::LinuxFileOps file_ops;
    auto root = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root.ok());

    CollectingSink sink;
    photobridge::LinuxDirectoryWalker walker;
    ASSERT_TRUE(walker.Walk(root.value().get(), sink).ok());

    ASSERT_EQ(sink.entries.size(), 4U);
    EXPECT_EQ(
        sink.entries.at("album"),
        photobridge::DirectoryEntryKind::kDirectory);
    EXPECT_EQ(
        sink.entries.at("album/photo.jpg"),
        photobridge::DirectoryEntryKind::kRegularFile);
    EXPECT_EQ(
        sink.entries.at("notes.txt"),
        photobridge::DirectoryEntryKind::kRegularFile);
    EXPECT_EQ(
        sink.entries.at("link.jpg"),
        photobridge::DirectoryEntryKind::kSymlink);
}

TEST_F(DirectoryWalkerTest, PropagatesSinkErrorAndStopsImmediately)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directory(root_, error));
    ASSERT_FALSE(error);

    {
        std::ofstream file(root_ / "photo.jpg");
        ASSERT_TRUE(file);
        file << "photo data";
    }

    photobridge::LinuxFileOps file_ops;
    auto root = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root.ok());

    FailingSink sink;
    photobridge::LinuxDirectoryWalker walker;
    const auto status = walker.Walk(root.value().get(), sink);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), photobridge::StatusCode::kInternal);
    EXPECT_EQ(status.message(), "sink stopped scan");
    EXPECT_EQ(sink.calls, 1);
}

TEST_F(DirectoryWalkerTest, RejectsInvalidRootFd)
{
    CollectingSink sink;
    photobridge::LinuxDirectoryWalker walker;

    const auto status = walker.Walk(-1, sink);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), photobridge::StatusCode::kInternal);
    EXPECT_TRUE(sink.entries.empty());
}

TEST_F(DirectoryWalkerTest, ReportsDirectoryThatDisappearsDuringScan)
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
    ASSERT_TRUE(root.ok()) << root.status().message();

    RemovingDirectorySink sink(root_);
    photobridge::LinuxDirectoryWalker walker;
    const photobridge::Status status =
        walker.Walk(root.value().get(), sink);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), photobridge::StatusCode::kNotFound);
}
