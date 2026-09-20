#include "photobridge/app/manifest_builder.h"

#include <fcntl.h>
#include <unistd.h>

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>

#include "photobridge/app/sqlite_schema.h"
#include "photobridge/filesystem/linux_directory_walker.h"
#include "photobridge/filesystem/linux_file_ops.h"
#include "photobridge/model/asset_classifier.h"

namespace {

std::size_t FixtureFileCount()
{
    constexpr std::size_t kDefault = 100'000;
    const char* raw_value = std::getenv("PHOTOBRIDGE_STRESS_FILE_COUNT");
    if (raw_value == nullptr) {
        return kDefault;
    }

    const std::string value(raw_value);
    std::size_t parsed = 0;
    const auto result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()
        || parsed == 0 || parsed > kDefault) {
        return kDefault;
    }
    return parsed;
}

class StreamingManifestSink final
    : public photobridge::DirectoryEntrySink {
public:
    explicit StreamingManifestSink(photobridge::ManifestBuilder& builder)
        : builder_(builder) {}

    photobridge::Status Add(photobridge::DirectoryEntry entry) override
    {
        auto asset = photobridge::ClassifyPhysicalAsset(entry);
        if (!asset.has_value()) {
            return photobridge::Status::Ok();
        }
        ++asset_count_;
        return builder_.Add(std::move(asset.value()));
    }

    std::size_t asset_count() const noexcept
    {
        return asset_count_;
    }

private:
    photobridge::ManifestBuilder& builder_;
    std::size_t asset_count_ = 0;
};

class LargeFixtureTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path()
            / ("photobridge_large_fixture_"
                + std::to_string(++sequence_));
        database_path_ = std::filesystem::temp_directory_path()
            / ("photobridge_large_fixture_"
                + std::to_string(sequence_)
                + ".db");
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::remove(database_path_, error);
        ASSERT_TRUE(std::filesystem::create_directory(root_, error));
        ASSERT_FALSE(error);
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::remove(database_path_, error);
    }

    std::filesystem::path root_;
    std::filesystem::path database_path_;
    static inline int sequence_ = 0;
};

TEST_F(LargeFixtureTest, ScansLargeFixtureThroughStreamingSink)
{
    const std::size_t file_count = FixtureFileCount();
    for (std::size_t index = 0; index < file_count; ++index) {
        const std::string name = "photo-"
            + std::to_string(index)
            + ".jpg";
        const int fd = ::open(
            (root_ / name).c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
            0600);
        ASSERT_GE(fd, 0) << name;
        ASSERT_EQ(::close(fd), 0);
    }

    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());

    photobridge::SqliteManifestBuilder builder(connection.value(), 256);
    ASSERT_TRUE(builder.Begin({
        "large-fixture",
        "local-folder",
        root_.string(),
    }).ok());

    photobridge::LinuxFileOps file_ops;
    auto root_fd = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root_fd.ok()) << root_fd.status().message();

    StreamingManifestSink sink(builder);
    photobridge::LinuxDirectoryWalker walker;
    const photobridge::Status walk_status =
        walker.Walk(root_fd.value().get(), sink);
    ASSERT_TRUE(walk_status.ok()) << walk_status.message();
    EXPECT_EQ(sink.asset_count(), file_count);

    auto frozen = builder.Freeze();
    ASSERT_TRUE(frozen.ok()) << frozen.status().message();
    EXPECT_EQ(frozen.value().asset_count, file_count);
}

}  // namespace
