#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "photobridge/lan/upload_session_store.h"

namespace {

class UploadSessionStoreTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path()
            / "photobridge-upload-session-test";
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

TEST_F(UploadSessionStoreTest, CreatesSessionAndAllocatesCollisionSuffixes)
{
    auto store = photobridge::UploadSessionStore::Open(root_);
    ASSERT_TRUE(store.ok()) << store.status().message();
    const std::vector<photobridge::UploadFileRequest> files{
        {"IMG_0001.JPG", 10, std::nullopt},
        {"IMG_0001.JPG", 20, std::nullopt},
    };

    auto session = store.value().CreateSession(files);
    ASSERT_TRUE(session.ok()) << session.status().message();
    ASSERT_EQ(session.value().files.size(), 2U);
    EXPECT_EQ(session.value().files[0].safe_filename, "IMG_0001.JPG");
    EXPECT_EQ(session.value().files[1].safe_filename, "IMG_0001__2.JPG");
    EXPECT_EQ(session.value().expected_total_bytes, 30U);

    auto loaded = store.value().GetSession(session.value().session_id);
    ASSERT_TRUE(loaded.ok()) << loaded.status().message();
    EXPECT_EQ(loaded.value().files.size(), 2U);
    EXPECT_EQ(loaded.value().files[1].file_id, session.value().files[1].file_id);
}

TEST_F(UploadSessionStoreTest, RejectsTraversalAndQuotaOverflow)
{
    auto store = photobridge::UploadSessionStore::Open(
        root_,
        photobridge::UploadQuota{100, 50, 100});
    ASSERT_TRUE(store.ok()) << store.status().message();

    auto traversal = store.value().CreateSession({
        {"../escape.jpg", 1, std::nullopt},
    });
    EXPECT_FALSE(traversal.ok());
    EXPECT_EQ(
        traversal.status().code(),
        photobridge::StatusCode::kInvalidArgument);

    auto quota = store.value().CreateSession({
        {"large.jpg", 51, std::nullopt},
    });
    EXPECT_FALSE(quota.ok());
}

TEST_F(UploadSessionStoreTest, AppliesTotalReservedQuota)
{
    auto store = photobridge::UploadSessionStore::Open(
        root_,
        photobridge::UploadQuota{100, 100, 10});
    ASSERT_TRUE(store.ok()) << store.status().message();

    auto first = store.value().CreateSession({
        {"first.jpg", 6, std::nullopt},
    });
    ASSERT_TRUE(first.ok()) << first.status().message();
    auto second = store.value().CreateSession({
        {"second.jpg", 5, std::nullopt},
    });
    EXPECT_FALSE(second.ok());
}

TEST_F(UploadSessionStoreTest, SerializesRetryAndRecognizesReceivedFile)
{
    auto store = photobridge::UploadSessionStore::Open(root_);
    ASSERT_TRUE(store.ok()) << store.status().message();
    auto session = store.value().CreateSession({
        {"photo.jpg", 4, std::nullopt},
    });
    ASSERT_TRUE(session.ok()) << session.status().message();
    const auto& file = session.value().files.front();

    auto started = store.value().BeginUpload(
        session.value().session_id, file.file_id);
    ASSERT_TRUE(started.ok());
    EXPECT_EQ(started.value(), photobridge::BeginUploadResult::kStarted);
    auto busy = store.value().BeginUpload(
        session.value().session_id, file.file_id);
    ASSERT_TRUE(busy.ok());
    EXPECT_EQ(busy.value(), photobridge::BeginUploadResult::kBusy);

    photobridge::Digest digest;
    digest.bytes[0] = static_cast<std::byte>(0x42);
    ASSERT_TRUE(store.value().CommitUpload(
        session.value().session_id,
        file.file_id,
        4,
        digest).ok());
    auto duplicate = store.value().BeginUpload(
        session.value().session_id, file.file_id);
    ASSERT_TRUE(duplicate.ok());
    EXPECT_EQ(
        duplicate.value(),
        photobridge::BeginUploadResult::kAlreadyReceived);
}

TEST_F(UploadSessionStoreTest, CompletesOnlyAfterEveryFileIsReceived)
{
    auto store = photobridge::UploadSessionStore::Open(root_);
    ASSERT_TRUE(store.ok()) << store.status().message();
    auto session = store.value().CreateSession({
        {"photo.jpg", 1, std::nullopt},
    });
    ASSERT_TRUE(session.ok()) << session.status().message();
    EXPECT_EQ(
        store.value().CompleteSession(session.value().session_id).code(),
        photobridge::StatusCode::kInvalidArgument);

    const auto& file = session.value().files.front();
    ASSERT_TRUE(store.value().BeginUpload(
        session.value().session_id, file.file_id).ok());
    photobridge::Digest digest;
    ASSERT_TRUE(store.value().CommitUpload(
        session.value().session_id, file.file_id, 1, digest).ok());
    ASSERT_TRUE(store.value().CompleteSession(session.value().session_id).ok());
    EXPECT_EQ(
        store.value().CompleteSession(session.value().session_id).code(),
        photobridge::StatusCode::kAlreadyExists);
}

TEST_F(UploadSessionStoreTest, ReopensAndAdoptsDurableFinalAfterInterruptedCommit)
{
    std::string session_id;
    std::string file_id;
    std::string safe_filename;
    {
        auto store = photobridge::UploadSessionStore::Open(root_);
        ASSERT_TRUE(store.ok()) << store.status().message();
        auto session = store.value().CreateSession({
            {"recovered.jpg", 3, std::nullopt},
        });
        ASSERT_TRUE(session.ok()) << session.status().message();
        session_id = session.value().session_id;
        file_id = session.value().files.front().file_id;
        safe_filename = session.value().files.front().safe_filename;
        ASSERT_TRUE(store.value().BeginUpload(session_id, file_id).ok());
        std::ofstream output(
            root_ / "incoming" / session_id / safe_filename,
            std::ios::binary);
        ASSERT_TRUE(output);
        output << "abc";
    }

    auto reopened = photobridge::UploadSessionStore::Open(root_);
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    auto file = reopened.value().GetFile(session_id, file_id);
    ASSERT_TRUE(file.ok()) << file.status().message();
    EXPECT_EQ(file.value().state, photobridge::UploadFileState::kReceived);
    EXPECT_EQ(file.value().received_size, 3U);
    EXPECT_TRUE(file.value().server_digest.has_value());
}

TEST_F(UploadSessionStoreTest, ReopensInterruptedTempAsRetryableFailure)
{
    std::string session_id;
    std::string file_id;
    {
        auto store = photobridge::UploadSessionStore::Open(root_);
        ASSERT_TRUE(store.ok()) << store.status().message();
        auto session = store.value().CreateSession({
            {"interrupted.jpg", 3, std::nullopt},
        });
        ASSERT_TRUE(session.ok()) << session.status().message();
        session_id = session.value().session_id;
        file_id = session.value().files.front().file_id;
        ASSERT_TRUE(store.value().BeginUpload(session_id, file_id).ok());
        std::ofstream output(
            store.value().TempPath(session_id, file_id),
            std::ios::binary);
        ASSERT_TRUE(output);
        output << "ab";
    }

    auto reopened = photobridge::UploadSessionStore::Open(root_);
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    auto file = reopened.value().GetFile(session_id, file_id);
    ASSERT_TRUE(file.ok()) << file.status().message();
    EXPECT_EQ(file.value().state, photobridge::UploadFileState::kFailed);
    EXPECT_TRUE(std::filesystem::exists(
        reopened.value().TempPath(session_id, file_id)));
}
