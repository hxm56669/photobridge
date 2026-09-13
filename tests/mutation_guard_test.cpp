#include "photobridge/filesystem/mutation_guard.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>

#include "photobridge/filesystem/linux_file_ops.h"

namespace {

class MutationGuardTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path()
            / ("photobridge_mutation_guard_"
                + std::to_string(++sequence_));
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        ASSERT_TRUE(std::filesystem::create_directory(root_, error));
        ASSERT_FALSE(error);
        std::ofstream file(root_ / "photo.jpg");
        ASSERT_TRUE(file);
        file << "original";
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    photobridge::PhysicalAsset AssetFromOpenFile(
        photobridge::LinuxFileOps& file_ops,
        photobridge::UniqueFd& root_fd)
    {
        auto path = photobridge::RelativePath::Parse("photo.jpg");
        EXPECT_TRUE(path.ok());
        auto file = file_ops.OpenSource(root_fd.get(), path.value());
        EXPECT_TRUE(file.ok());
        auto identity = file_ops.StatFd(file.value().get());
        EXPECT_TRUE(identity.ok());
        return photobridge::PhysicalAsset{
            std::move(path.value()),
            identity.value(),
            photobridge::AssetKind::kMedia,
            "jpg",
        };
    }

    std::filesystem::path root_;
    static inline int sequence_ = 0;
};

TEST_F(MutationGuardTest, OpensAndVerifiesStableFileIdentity)
{
    photobridge::LinuxFileOps file_ops;
    auto root_fd = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root_fd.ok()) << root_fd.status().message();
    const auto asset = AssetFromOpenFile(file_ops, root_fd.value());

    auto guard = photobridge::MutationGuard::Open(
        file_ops,
        root_fd.value().get(),
        asset);
    ASSERT_TRUE(guard.ok()) << guard.status().message();
    EXPECT_GE(guard.value().fd(), 0);
    EXPECT_TRUE(guard.value().VerifyBeforeRead().ok());
    EXPECT_TRUE(guard.value().VerifyAfterRead().ok());
}

TEST_F(MutationGuardTest, RejectsFileChangedThroughOpenedDescriptor)
{
    photobridge::LinuxFileOps file_ops;
    auto root_fd = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root_fd.ok()) << root_fd.status().message();
    const auto asset = AssetFromOpenFile(file_ops, root_fd.value());
    auto guard = photobridge::MutationGuard::Open(
        file_ops,
        root_fd.value().get(),
        asset);
    ASSERT_TRUE(guard.ok()) << guard.status().message();

    std::ofstream file(root_ / "photo.jpg", std::ios::trunc);
    ASSERT_TRUE(file);
    file << "changed content";
    file.close();

    EXPECT_FALSE(guard.value().VerifyAfterRead().ok());
}

TEST_F(MutationGuardTest, RejectsFileChangedBeforeGuardOpens)
{
    photobridge::LinuxFileOps file_ops;
    auto root_fd = file_ops.OpenRoot(
        root_,
        photobridge::OpenRootMode::kExisting);
    ASSERT_TRUE(root_fd.ok()) << root_fd.status().message();
    const auto asset = AssetFromOpenFile(file_ops, root_fd.value());

    std::ofstream file(root_ / "photo.jpg", std::ios::trunc);
    ASSERT_TRUE(file);
    file << "changed content";
    file.close();

    const auto guard = photobridge::MutationGuard::Open(
        file_ops,
        root_fd.value().get(),
        asset);
    ASSERT_FALSE(guard.ok());
    EXPECT_EQ(guard.status().code(), photobridge::StatusCode::kInternal);
}

}  // namespace
