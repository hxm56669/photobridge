#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/filesystem/binary_verifier.h"
#include "photobridge/filesystem/copy_and_hash.h"

namespace {

class VerifyFileOps final : public photobridge::FileOps {
public:
    explicit VerifyFileOps(std::string data)
        : data_(data.size())
    {
        std::transform(
            data.begin(),
            data.end(),
            data_.begin(),
            [](char value) { return static_cast<std::byte>(value); });
        before_.inode = 7;
        before_.size = data_.size();
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
        std::string_view,
        mode_t) override
    {
        return Unused();
    }

    photobridge::StatusOr<std::size_t> Read(
        int,
        std::span<std::byte> buffer) override
    {
        const std::size_t remaining = data_.size() - offset_;
        const std::size_t count = std::min(remaining, std::min(buffer.size(), read_chunk_));
        std::copy_n(data_.begin() + offset_, count, buffer.begin());
        offset_ += count;
        return count;
    }

    photobridge::StatusOr<std::size_t> Write(
        int,
        std::span<const std::byte>) override
    {
        return std::size_t{0};
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

    photobridge::FileIdentity& after() { return after_; }

private:
    static photobridge::StatusOr<photobridge::UniqueFd> Unused()
    {
        return photobridge::Status(
            photobridge::StatusCode::kInternal,
            "unused FileOps operation");
    }

    std::vector<std::byte> data_;
    std::size_t offset_ = 0;
    std::size_t stat_calls_ = 0;
    std::size_t read_chunk_ = 3;
    photobridge::FileIdentity before_;
    photobridge::FileIdentity after_;
};

photobridge::Digest DigestFor(std::string_view value)
{
    photobridge::Blake3Hasher hasher;
    std::vector<std::byte> bytes(value.size());
    std::transform(
        value.begin(),
        value.end(),
        bytes.begin(),
        [](char byte) { return static_cast<std::byte>(byte); });
    EXPECT_TRUE(hasher.Update(bytes).ok());
    const auto digest = hasher.Finalize();
    EXPECT_TRUE(digest.ok());
    return digest.value();
}

}  // namespace

TEST(BinaryVerifierTest, IndependentlyReadsAndRecognizesIdenticalTarget)
{
    constexpr std::string_view payload = "target bytes";
    VerifyFileOps file_ops{std::string(payload)};
    photobridge::Blake3Hasher hasher;
    std::vector<std::byte> buffer(4);

    const auto result = photobridge::VerifyBinary(
        file_ops,
        hasher,
        20,
        payload.size(),
        DigestFor(payload),
        buffer);
    ASSERT_TRUE(result.ok()) << result.status().message();
    EXPECT_EQ(
        result.value().status,
        photobridge::BinaryVerification::kIdentical);
    EXPECT_EQ(result.value().bytes_read, payload.size());
}

TEST(BinaryVerifierTest, ReportsMismatchAndNotApplicableSeparately)
{
    constexpr std::string_view payload = "target bytes";
    VerifyFileOps mismatch_ops{std::string(payload)};
    photobridge::Blake3Hasher mismatch_hasher;
    std::vector<std::byte> buffer(5);
    auto wrong_digest = photobridge::Digest{};
    const auto mismatch = photobridge::VerifyBinary(
        mismatch_ops,
        mismatch_hasher,
        20,
        payload.size() + 1,
        wrong_digest,
        buffer);
    ASSERT_TRUE(mismatch.ok());
    EXPECT_EQ(
        mismatch.value().status,
        photobridge::BinaryVerification::kMismatch);

    VerifyFileOps not_applicable_ops{std::string(payload)};
    photobridge::Blake3Hasher not_applicable_hasher;
    const auto not_applicable = photobridge::VerifyBinary(
        not_applicable_ops,
        not_applicable_hasher,
        20,
        payload.size(),
        std::nullopt,
        buffer);
    ASSERT_TRUE(not_applicable.ok());
    EXPECT_EQ(
        not_applicable.value().status,
        photobridge::BinaryVerification::kNotApplicable);
}

TEST(BinaryVerifierTest, RejectsTargetMutationAndEmptyBuffer)
{
    VerifyFileOps mutated_ops("target bytes");
    mutated_ops.after().inode = 8;
    photobridge::Blake3Hasher mutated_hasher;
    std::vector<std::byte> buffer(4);
    const auto mutated = photobridge::VerifyBinary(
        mutated_ops,
        mutated_hasher,
        20,
        12,
        DigestFor("target bytes"),
        buffer);
    EXPECT_FALSE(mutated.ok());
    EXPECT_EQ(mutated.status().code(), photobridge::StatusCode::kInternal);

    VerifyFileOps empty_ops("target bytes");
    photobridge::Blake3Hasher empty_hasher;
    std::vector<std::byte> empty_buffer;
    const auto empty = photobridge::VerifyBinary(
        empty_ops,
        empty_hasher,
        20,
        12,
        std::nullopt,
        empty_buffer);
    EXPECT_FALSE(empty.ok());
    EXPECT_EQ(
        empty.status().code(),
        photobridge::StatusCode::kInvalidArgument);
}
