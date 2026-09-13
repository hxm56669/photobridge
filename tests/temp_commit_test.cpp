#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/filesystem/temp_commit.h"

namespace {

class CommitFileOps final : public photobridge::FileOps {
public:
    explicit CommitFileOps(std::string data)
        : source_(data.size())
    {
        std::transform(
            data.begin(),
            data.end(),
            source_.begin(),
            [](char value) { return static_cast<std::byte>(value); });
        before_.inode = 1;
        before_.size = source_.size();
        after_ = before_;
    }

    photobridge::StatusOr<photobridge::UniqueFd> OpenRoot(
        const std::filesystem::path&,
        photobridge::OpenRootMode) override
    {
        return Unused();
    }

    photobridge::StatusOr<photobridge::UniqueFd> OpenSource(
        int,
        const photobridge::RelativePath&) override
    {
        return Unused();
    }

    photobridge::StatusOr<photobridge::FileIdentity> StatFd(int) override
    {
        return stat_calls_++ == 0 ? before_ : after_;
    }

    photobridge::StatusOr<photobridge::UniqueFd> CreateTempNoReplace(
        int,
        std::string_view name,
        mode_t) override
    {
        if (create_error_) {
            return photobridge::Status(
                photobridge::StatusCode::kAlreadyExists,
                "temporary already exists");
        }
        temp_name_ = std::string(name);
        return photobridge::UniqueFd(100);
    }

    photobridge::StatusOr<std::size_t> Read(
        int,
        std::span<std::byte> buffer) override
    {
        const std::size_t remaining = source_.size() - offset_;
        const std::size_t count = std::min(remaining, std::min(buffer.size(), read_chunk_));
        std::copy_n(source_.begin() + offset_, count, buffer.begin());
        offset_ += count;
        return count;
    }

    photobridge::StatusOr<std::size_t> Write(
        int,
        std::span<const std::byte> buffer) override
    {
        if (write_zero_) return std::size_t{0};
        const std::size_t count = std::min(buffer.size(), write_chunk_);
        target_.insert(target_.end(), buffer.begin(), buffer.begin() + count);
        return count;
    }

    photobridge::Status Fdatasync(int) override
    {
        ++fdatasync_calls_;
        return fdatasync_error_ ? Failure() : photobridge::Status::Ok();
    }

    photobridge::Status FsyncDirectory(int) override
    {
        ++fsync_directory_calls_;
        return fsync_error_ ? Failure() : photobridge::Status::Ok();
    }

    photobridge::Status RenameNoReplace(
        int,
        std::string_view old_name,
        int,
        std::string_view new_name) override
    {
        renamed_from_ = std::string(old_name);
        renamed_to_ = std::string(new_name);
        return rename_error_ ? Failure() : photobridge::Status::Ok();
    }

    photobridge::Status UnlinkAt(int, std::string_view name) override
    {
        unlinked_name_ = std::string(name);
        return photobridge::Status::Ok();
    }

    void set_write_zero(bool value) { write_zero_ = value; }
    void set_create_error(bool value) { create_error_ = value; }
    void set_fdatasync_error(bool value) { fdatasync_error_ = value; }
    void set_fsync_error(bool value) { fsync_error_ = value; }
    void set_rename_error(bool value) { rename_error_ = value; }
    const std::vector<std::byte>& target() const { return target_; }
    const std::string& temp_name() const { return temp_name_; }
    const std::string& unlinked_name() const { return unlinked_name_; }
    int fdatasync_calls() const { return fdatasync_calls_; }
    int fsync_directory_calls() const { return fsync_directory_calls_; }

private:
    static photobridge::StatusOr<photobridge::UniqueFd> Unused()
    {
        return photobridge::Status(
            photobridge::StatusCode::kInternal,
            "unused FileOps operation");
    }

    static photobridge::Status Failure()
    {
        return photobridge::Status(
            photobridge::StatusCode::kIoError,
            "injected commit failure");
    }

    std::vector<std::byte> source_;
    std::vector<std::byte> target_;
    std::size_t offset_ = 0;
    std::size_t stat_calls_ = 0;
    std::size_t read_chunk_ = 3;
    std::size_t write_chunk_ = 2;
    bool write_zero_ = false;
    bool create_error_ = false;
    bool fdatasync_error_ = false;
    bool fsync_error_ = false;
    bool rename_error_ = false;
    photobridge::FileIdentity before_;
    photobridge::FileIdentity after_;
    std::string temp_name_;
    std::string renamed_from_;
    std::string renamed_to_;
    std::string unlinked_name_;
    int fdatasync_calls_ = 0;
    int fsync_directory_calls_ = 0;
};

}  // namespace

TEST(TempCommitTest, SyncsAndPublishesWithNoReplace)
{
    CommitFileOps file_ops("payload");
    photobridge::Blake3Hasher hasher;
    std::vector<std::byte> buffer(4);

    const auto result = photobridge::CopyToTempAndPublish(
        file_ops,
        hasher,
        10,
        20,
        ".photo.pbtmp",
        "photo.jpg",
        buffer);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().temp_name, ".photo.pbtmp");
    EXPECT_EQ(result.value().final_name, "photo.jpg");
    EXPECT_EQ(file_ops.temp_name(), ".photo.pbtmp");
    EXPECT_EQ(file_ops.unlinked_name(), "");
    EXPECT_EQ(file_ops.fdatasync_calls(), 1);
    EXPECT_EQ(file_ops.fsync_directory_calls(), 1);
}

TEST(TempCommitTest, CleansTempBeforePublishWhenCopyOrSyncFails)
{
    CommitFileOps file_ops("payload");
    file_ops.set_write_zero(true);
    photobridge::Blake3Hasher hasher;
    std::vector<std::byte> buffer(4);

    const auto result = photobridge::CopyToTempAndPublish(
        file_ops,
        hasher,
        10,
        20,
        ".photo.pbtmp",
        "photo.jpg",
        buffer);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(file_ops.unlinked_name(), ".photo.pbtmp");
}

TEST(TempCommitTest, PreservesTempForRenameRecoveryWhenPublishFails)
{
    CommitFileOps file_ops("payload");
    file_ops.set_rename_error(true);
    photobridge::Blake3Hasher hasher;
    std::vector<std::byte> buffer(4);

    const auto result = photobridge::CopyToTempAndPublish(
        file_ops,
        hasher,
        10,
        20,
        ".photo.pbtmp",
        "photo.jpg",
        buffer);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(file_ops.unlinked_name(), "");
    EXPECT_EQ(file_ops.fdatasync_calls(), 1);
    EXPECT_EQ(file_ops.fsync_directory_calls(), 0);
}

TEST(TempCommitTest, FdatasyncFailureStopsPublishAndCleansTemp)
{
    CommitFileOps file_ops("payload");
    file_ops.set_fdatasync_error(true);
    photobridge::Blake3Hasher hasher;
    std::vector<std::byte> buffer(4);

    const auto result = photobridge::CopyToTempAndPublish(
        file_ops,
        hasher,
        10,
        20,
        ".photo.pbtmp",
        "photo.jpg",
        buffer);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), photobridge::StatusCode::kIoError);
    EXPECT_EQ(file_ops.unlinked_name(), ".photo.pbtmp");
    EXPECT_EQ(file_ops.fsync_directory_calls(), 0);
}

TEST(TempCommitTest, DirectorySyncFailureDoesNotReportCommitted)
{
    CommitFileOps file_ops("payload");
    file_ops.set_fsync_error(true);
    photobridge::Blake3Hasher hasher;
    std::vector<std::byte> buffer(4);

    const auto result = photobridge::CopyToTempAndPublish(
        file_ops,
        hasher,
        10,
        20,
        ".photo.pbtmp",
        "photo.jpg",
        buffer);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), photobridge::StatusCode::kIoError);
    EXPECT_EQ(file_ops.fsync_directory_calls(), 1);
}

TEST(TempCommitTest, RejectsInvalidNamesBeforeCreatingTemp)
{
    CommitFileOps file_ops("payload");
    photobridge::Blake3Hasher hasher;
    std::vector<std::byte> buffer(4);

    const auto result = photobridge::CopyToTempAndPublish(
        file_ops,
        hasher,
        10,
        20,
        "",
        "photo.jpg",
        buffer);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(
        result.status().code(),
        photobridge::StatusCode::kInvalidArgument);
    EXPECT_EQ(file_ops.temp_name(), "");
}
