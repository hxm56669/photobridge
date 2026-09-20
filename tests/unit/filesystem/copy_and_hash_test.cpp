#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/filesystem/copy_and_hash.h"

namespace {

class FakeFileOps final : public photobridge::FileOps {
public:
    explicit FakeFileOps(std::string data)
        : source_(data.size())
    {
        std::transform(
            data.begin(),
            data.end(),
            source_.begin(),
            [](char value) { return static_cast<std::byte>(value); });
        before_.inode = 10;
        before_.size = source_.size();
        after_ = before_;
    }

    photobridge::StatusOr<photobridge::UniqueFd> OpenRoot(
        const std::filesystem::path&,
        photobridge::OpenRootMode) override
    {
        return NotImplemented();
    }

    photobridge::StatusOr<photobridge::UniqueFd> OpenSource(
        int,
        const photobridge::RelativePath&) override
    {
        return NotImplemented();
    }

    photobridge::StatusOr<photobridge::FileIdentity> StatFd(int) override
    {
        ++stat_calls_;
        return stat_calls_ == 1 ? before_ : after_;
    }

    photobridge::StatusOr<photobridge::UniqueFd> CreateTempNoReplace(
        int,
        std::string_view,
        mode_t) override
    {
        return NotImplemented();
    }

    photobridge::StatusOr<std::size_t> Read(
        int,
        std::span<std::byte> buffer) override
    {
        const std::size_t remaining = source_.size() - offset_;
        const std::size_t count = std::min({remaining, buffer.size(), read_chunk_});
        std::copy_n(source_.begin() + offset_, count, buffer.begin());
        offset_ += count;
        return count;
    }

    photobridge::StatusOr<std::size_t> Write(
        int,
        std::span<const std::byte> buffer) override
    {
        if (write_zero_) {
            return std::size_t{0};
        }
        const std::size_t count = std::min(buffer.size(), write_chunk_);
        target_.insert(target_.end(), buffer.begin(), buffer.begin() + count);
        return count;
    }

    photobridge::Status Fdatasync(int) override { return photobridge::Status::Ok(); }
    photobridge::Status FsyncDirectory(int) override { return photobridge::Status::Ok(); }

    photobridge::Status RenameNoReplace(
        int,
        std::string_view,
        int,
        std::string_view) override
    {
        return photobridge::Status::Ok();
    }

    photobridge::Status UnlinkAt(int, std::string_view) override
    {
        return photobridge::Status::Ok();
    }

    const std::vector<std::byte>& target() const { return target_; }
    photobridge::FileIdentity& after() { return after_; }
    void set_read_chunk(std::size_t value) { read_chunk_ = value; }
    void set_write_chunk(std::size_t value) { write_chunk_ = value; }
    void set_write_zero(bool value) { write_zero_ = value; }

private:
    static photobridge::StatusOr<photobridge::UniqueFd> NotImplemented()
    {
        return photobridge::Status(
            photobridge::StatusCode::kInternal,
            "not used by CopyAndHash test");
    }

    std::vector<std::byte> source_;
    std::vector<std::byte> target_;
    std::size_t offset_ = 0;
    std::size_t stat_calls_ = 0;
    std::size_t read_chunk_ = 3;
    std::size_t write_chunk_ = 2;
    bool write_zero_ = false;
    photobridge::FileIdentity before_;
    photobridge::FileIdentity after_;
};

std::vector<std::byte> Bytes(std::string_view value)
{
    std::vector<std::byte> result(value.size());
    std::transform(
        value.begin(),
        value.end(),
        result.begin(),
        [](char byte) { return static_cast<std::byte>(byte); });
    return result;
}

}  // namespace

TEST(CopyAndHashTest, HandlesShortReadsAndShortWrites)
{
    FakeFileOps file_ops("PhotoBridge copy payload");
    photobridge::Blake3Hasher hasher;
    std::vector<std::byte> buffer(5);

    const auto result = photobridge::CopyAndHash(
        file_ops,
        hasher,
        10,
        20,
        buffer);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().bytes_copied, 24U);
    EXPECT_EQ(file_ops.target(), Bytes("PhotoBridge copy payload"));

    photobridge::Blake3Hasher expected_hasher;
    const auto expected_bytes = Bytes("PhotoBridge copy payload");
    ASSERT_TRUE(expected_hasher.Update(expected_bytes).ok());
    const auto expected = expected_hasher.Finalize();
    ASSERT_TRUE(expected.ok());
    EXPECT_EQ(result.value().source_digest, expected.value());
}

TEST(CopyAndHashTest, RejectsZeroProgressWrites)
{
    FakeFileOps file_ops("payload");
    file_ops.set_write_zero(true);
    photobridge::Blake3Hasher hasher;
    std::vector<std::byte> buffer(8);

    const auto result = photobridge::CopyAndHash(
        file_ops,
        hasher,
        10,
        20,
        buffer);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.status().code(), photobridge::StatusCode::kIoError);
}

TEST(CopyAndHashTest, RejectsEmptyBuffer)
{
    FakeFileOps file_ops("payload");
    photobridge::Blake3Hasher hasher;
    std::vector<std::byte> buffer;

    const auto result = photobridge::CopyAndHash(
        file_ops,
        hasher,
        10,
        20,
        buffer);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(
        result.status().code(),
        photobridge::StatusCode::kInvalidArgument);
}

TEST(CopyAndHashTest, StreamsLargePayloadWithoutChangingContract)
{
    const std::string payload(4U * 1024U * 1024U, 'p');
    FakeFileOps file_ops(payload);
    photobridge::Blake3Hasher hasher;
    std::vector<std::byte> buffer(64U * 1024U);

    const auto result = photobridge::CopyAndHash(
        file_ops,
        hasher,
        10,
        20,
        buffer);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(result.value().bytes_copied, payload.size());
    EXPECT_EQ(file_ops.target(), Bytes(payload));
}
