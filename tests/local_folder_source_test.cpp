#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/source/local_folder_source.h"

namespace {

class CollectingSink final : public photobridge::PhysicalAssetSink {
public:
    photobridge::Status Add(photobridge::PhysicalAsset asset) override
    {
        paths.push_back(std::string(asset.relative_path.bytes()));
        return photobridge::Status::Ok();
    }

    std::vector<std::string> paths;
};

class LocalFolderSourceTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path()
            / ("photobridge_local_folder_source_"
                + std::to_string(++sequence_));
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path root_;
    static inline int sequence_ = 0;
};

}  // namespace

TEST_F(LocalFolderSourceTest, StreamsRegularFilesInStablePathOrder)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(root_ / "nested", error));
    ASSERT_FALSE(error);

    {
        std::ofstream file(root_ / "b.jpg");
        ASSERT_TRUE(file);
        file << "b";
    }
    {
        std::ofstream file(root_ / "a.jpg");
        ASSERT_TRUE(file);
        file << "a";
    }
    {
        std::ofstream file(root_ / "nested" / "c.json");
        ASSERT_TRUE(file);
        file << "c";
    }
    {
        std::ofstream file(root_ / "ignored.bin");
        ASSERT_TRUE(file);
        file << "ignored";
    }

    photobridge::LocalFolderSource source(root_);
    CollectingSink sink;

    ASSERT_TRUE(source.Scan(sink).ok());
    EXPECT_EQ(
        sink.paths,
        (std::vector<std::string>{
            "a.jpg",
            "b.jpg",
            "ignored.bin",
            "nested/c.json",
        }));
    EXPECT_EQ(source.TypeName(), "local-folder");
}

TEST_F(LocalFolderSourceTest, ReportsMissingRoot)
{
    photobridge::LocalFolderSource source(root_);
    CollectingSink sink;

    const photobridge::Status status = source.Scan(sink);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), photobridge::StatusCode::kNotFound);
    EXPECT_TRUE(sink.paths.empty());
}

TEST_F(LocalFolderSourceTest, IgnoresReceiverTemporaryFiles)
{
    std::error_code error;
    ASSERT_TRUE(std::filesystem::create_directories(
        root_ / "incoming" / ".tmp" / "session", error));
    ASSERT_FALSE(error);
    {
        std::ofstream file(root_ / "incoming" / ".tmp" / "session" / "file.pbtmp");
        ASSERT_TRUE(file);
        file << "partial";
    }
    {
        std::ofstream file(root_ / "photo.jpg");
        ASSERT_TRUE(file);
        file << "complete";
    }

    photobridge::LocalFolderSource source(root_);
    CollectingSink sink;
    ASSERT_TRUE(source.Scan(sink).ok());
    EXPECT_EQ(
        sink.paths,
        (std::vector<std::string>{"photo.jpg"}));
}
