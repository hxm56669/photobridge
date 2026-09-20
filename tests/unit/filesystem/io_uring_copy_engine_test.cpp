#include <cstdio>
#include <memory>
#include <span>
#include <string>

#include <gtest/gtest.h>

#include "photobridge/filesystem/io_uring_copy_engine.h"

namespace {

using TemporaryFile = std::unique_ptr<FILE, decltype(&std::fclose)>;

TemporaryFile OpenTemporaryFile()
{
    return TemporaryFile(std::tmpfile(), &std::fclose);
}

TEST(IoUringCopyEngineTest, CopiesMoreThanFourBuffersAndPreservesHashOrder)
{
    auto source = OpenTemporaryFile();
    auto target = OpenTemporaryFile();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(target, nullptr);
    std::string payload(9U * 1024U * 1024U + 137U, '\0');
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<char>(index % 251U);
    }
    ASSERT_EQ(std::fwrite(payload.data(), 1, payload.size(), source.get()),
              payload.size());
    ASSERT_EQ(std::fflush(source.get()), 0);

    photobridge::Blake3Hasher hasher;
    auto copied = photobridge::TryIoUringCopyAndHash(
        hasher, fileno(source.get()), fileno(target.get()), payload.size());
    ASSERT_TRUE(copied.ok()) << copied.status().message();
    if (!copied.value().has_value()) GTEST_SKIP() << "io_uring unavailable";
    EXPECT_EQ(copied.value()->bytes_copied, payload.size());

    photobridge::Blake3Hasher expected_hasher;
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size());
    ASSERT_TRUE(expected_hasher.Update(bytes).ok());
    const auto expected_digest = expected_hasher.Finalize();
    ASSERT_TRUE(expected_digest.ok());
    EXPECT_EQ(copied.value()->source_digest, expected_digest.value());

    std::rewind(target.get());
    std::string output(payload.size(), '\0');
    ASSERT_EQ(std::fread(output.data(), 1, output.size(), target.get()),
              output.size());
    EXPECT_EQ(output, payload);
}

TEST(IoUringCopyEngineTest, RejectsPrematureSourceEnd)
{
    auto source = OpenTemporaryFile();
    auto target = OpenTemporaryFile();
    ASSERT_NE(source, nullptr);
    ASSERT_NE(target, nullptr);
    const std::string payload(1024, 'x');
    ASSERT_EQ(std::fwrite(payload.data(), 1, payload.size(), source.get()),
              payload.size());
    ASSERT_EQ(std::fflush(source.get()), 0);

    photobridge::Blake3Hasher hasher;
    auto copied = photobridge::TryIoUringCopyAndHash(
        hasher, fileno(source.get()), fileno(target.get()), 2048, 2048);
    ASSERT_TRUE(copied.ok() || copied.status().code()
        == photobridge::StatusCode::kIoError);
    if (copied.ok()) {
        GTEST_SKIP() << "io_uring unavailable";
    }
    EXPECT_EQ(copied.status().code(), photobridge::StatusCode::kIoError);
}

TEST(IoUringCopyEngineTest, ReportsNegativeWriteCompletion)
{
    auto source = OpenTemporaryFile();
    TemporaryFile target(std::fopen("/dev/full", "wb"), &std::fclose);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(target, nullptr);
    const std::string payload(4096, 'x');
    ASSERT_EQ(std::fwrite(payload.data(), 1, payload.size(), source.get()),
              payload.size());
    ASSERT_EQ(std::fflush(source.get()), 0);

    photobridge::Blake3Hasher hasher;
    auto copied = photobridge::TryIoUringCopyAndHash(
        hasher, fileno(source.get()), fileno(target.get()), payload.size());
    if (copied.ok()) GTEST_SKIP() << "io_uring unavailable";
    EXPECT_EQ(copied.status().code(), photobridge::StatusCode::kIoError);
}

}  // namespace
