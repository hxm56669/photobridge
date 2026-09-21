#include "photobridge/app/manifest_builder.h"

#include <sqlite3.h>
#include <unistd.h>

#include <filesystem>
#include <cstdint>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "photobridge/app/sqlite_schema.h"

namespace {

class ManifestBuilderTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        database_path_ = std::filesystem::temp_directory_path()
            / ("photobridge_manifest_builder_"
                + std::to_string(::getpid()) + "_"
                + std::to_string(++sequence_)
                + ".db");
        std::error_code error;
        std::filesystem::remove(database_path_, error);
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove(database_path_, error);
    }

    std::filesystem::path database_path_;
    static inline int sequence_ = 0;
};

photobridge::PhysicalAsset MakeAsset(
    std::string path,
    std::uint64_t inode)
{
    auto relative_path = photobridge::RelativePath::Parse(std::move(path));
    EXPECT_TRUE(relative_path.ok());
    photobridge::FileIdentity identity;
    identity.device = 7;
    identity.inode = inode;
    identity.size = 123;
    identity.mtime_ns = 456;
    identity.ctime_ns = 789;
    return photobridge::PhysicalAsset{
        std::move(relative_path.value()),
        identity,
        photobridge::AssetKind::kMedia,
        "jpg",
    };
}

TEST_F(ManifestBuilderTest, WritesAssetsInCommittedBatchesAndFreezes)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());

    photobridge::SqliteManifestBuilder builder(connection.value(), 2);
    ASSERT_TRUE(builder.Begin({"source-1", "local-folder", "/photos"}).ok());
    EXPECT_EQ(
        builder.state(),
        photobridge::ManifestBuilderState::kBuilding);

    ASSERT_TRUE(builder.Add(MakeAsset("b/photo.jpg", 2)).ok());
    EXPECT_EQ(builder.state(), photobridge::ManifestBuilderState::kBuilding);
    ASSERT_TRUE(builder.Add(MakeAsset("a/photo.jpg", 1)).ok());

    auto frozen = builder.Freeze();
    ASSERT_TRUE(frozen.ok()) << frozen.status().message();
    EXPECT_EQ(frozen.value().asset_count, 2U);
    EXPECT_EQ(builder.state(), photobridge::ManifestBuilderState::kFrozen);

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            connection.value().native_handle(),
            "SELECT state, source_id, source_root FROM source_manifest;",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0), 1);
    EXPECT_STREQ(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)),
        "source-1");
    EXPECT_EQ(
        std::string(
            reinterpret_cast<const char*>(sqlite3_column_blob(statement, 2)),
            sqlite3_column_bytes(statement, 2)),
        "/photos");
    EXPECT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);

    ASSERT_EQ(
        sqlite3_prepare_v2(
            connection.value().native_handle(),
            "SELECT COUNT(*) FROM physical_asset WHERE manifest_id = ?;",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(
        sqlite3_bind_text(
            statement,
            1,
            frozen.value().manifest_id.c_str(),
            -1,
            SQLITE_TRANSIENT),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0), 2);
    sqlite3_finalize(statement);
}

TEST_F(ManifestBuilderTest, RejectsDuplicatePathWithoutPartialBatch)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());

    photobridge::SqliteManifestBuilder builder(connection.value(), 2);
    ASSERT_TRUE(builder.Begin({"source-1", "local-folder", "/photos"}).ok());
    ASSERT_TRUE(builder.Add(MakeAsset("same.jpg", 1)).ok());
    const photobridge::Status status =
        builder.Add(MakeAsset("same.jpg", 2));
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), photobridge::StatusCode::kAlreadyExists);

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            connection.value().native_handle(),
            "SELECT COUNT(*) FROM physical_asset;",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(statement, 0), 0);
    sqlite3_finalize(statement);
}

TEST_F(ManifestBuilderTest, RejectsAddAfterFreeze)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());

    photobridge::SqliteManifestBuilder builder(connection.value());
    ASSERT_TRUE(builder.Begin({"source-1", "local-folder", "/photos"}).ok());
    ASSERT_TRUE(builder.Freeze().ok());

    const photobridge::Status status =
        builder.Add(MakeAsset("later.jpg", 1));
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), photobridge::StatusCode::kInvalidArgument);
}

TEST_F(ManifestBuilderTest, DigestIgnoresScanOrderAndManifestId)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());

    photobridge::SqliteManifestBuilder first(connection.value(), 2);
    ASSERT_TRUE(first.Begin({"source-1", "local-folder", "/photos"}).ok());
    ASSERT_TRUE(first.Add(MakeAsset("b/photo.jpg", 2)).ok());
    ASSERT_TRUE(first.Add(MakeAsset("a/photo.jpg", 1)).ok());
    auto first_frozen = first.Freeze();
    ASSERT_TRUE(first_frozen.ok()) << first_frozen.status().message();

    photobridge::SqliteManifestBuilder second(connection.value(), 1);
    ASSERT_TRUE(second.Begin({"source-1", "local-folder", "/photos"}).ok());
    ASSERT_TRUE(second.Add(MakeAsset("a/photo.jpg", 1)).ok());
    ASSERT_TRUE(second.Add(MakeAsset("b/photo.jpg", 2)).ok());
    auto second_frozen = second.Freeze();
    ASSERT_TRUE(second_frozen.ok()) << second_frozen.status().message();

    EXPECT_NE(
        first_frozen.value().manifest_id,
        second_frozen.value().manifest_id);
    EXPECT_EQ(
        first_frozen.value().manifest_digest,
        second_frozen.value().manifest_digest);
    EXPECT_EQ(first_frozen.value().manifest_digest.ToHex().size(), 64U);
}

TEST_F(ManifestBuilderTest, DigestChangesWhenFileIdentityChanges)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());

    photobridge::SqliteManifestBuilder first(connection.value());
    ASSERT_TRUE(first.Begin({"source-1", "local-folder", "/photos"}).ok());
    ASSERT_TRUE(first.Add(MakeAsset("photo.jpg", 1)).ok());
    auto first_frozen = first.Freeze();
    ASSERT_TRUE(first_frozen.ok()) << first_frozen.status().message();

    photobridge::SqliteManifestBuilder second(connection.value());
    ASSERT_TRUE(second.Begin({"source-1", "local-folder", "/photos"}).ok());
    ASSERT_TRUE(second.Add(MakeAsset("photo.jpg", 2)).ok());
    auto second_frozen = second.Freeze();
    ASSERT_TRUE(second_frozen.ok()) << second_frozen.status().message();

    EXPECT_NE(
        first_frozen.value().manifest_digest,
        second_frozen.value().manifest_digest);
}

TEST_F(ManifestBuilderTest, ReopensFrozenManifestAndRejectsMutation)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());

    photobridge::SqliteManifestBuilder builder(connection.value());
    ASSERT_TRUE(builder.Begin({"source-1", "local-folder", "/photos"}).ok());
    ASSERT_TRUE(builder.Add(MakeAsset("photo.jpg", 1)).ok());
    auto frozen = builder.Freeze();
    ASSERT_TRUE(frozen.ok()) << frozen.status().message();

    auto reopened = photobridge::SqliteManifestBuilder::Reopen(
        connection.value(),
        frozen.value().manifest_id);
    ASSERT_TRUE(reopened.ok()) << reopened.status().message();
    EXPECT_EQ(
        reopened.value().state(),
        photobridge::ManifestBuilderState::kFrozen);

    auto reopened_manifest = reopened.value().frozen_manifest();
    ASSERT_TRUE(reopened_manifest.ok())
        << reopened_manifest.status().message();
    EXPECT_EQ(reopened_manifest.value().manifest_id, frozen.value().manifest_id);
    EXPECT_EQ(reopened_manifest.value().asset_count, 1U);
    EXPECT_EQ(
        reopened_manifest.value().manifest_digest,
        frozen.value().manifest_digest);

    EXPECT_EQ(
        reopened.value().Add(MakeAsset("later.jpg", 2)).code(),
        photobridge::StatusCode::kInvalidArgument);
    EXPECT_FALSE(reopened.value().Freeze().ok());
}

TEST_F(ManifestBuilderTest, ReopenRejectsManifestDigestMismatch)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());

    photobridge::SqliteManifestBuilder builder(connection.value());
    ASSERT_TRUE(builder.Begin({"source-1", "local-folder", "/photos"}).ok());
    ASSERT_TRUE(builder.Add(MakeAsset("photo.jpg", 1)).ok());
    auto frozen = builder.Freeze();
    ASSERT_TRUE(frozen.ok()) << frozen.status().message();
    ASSERT_TRUE(connection.value().Execute(
        "UPDATE physical_asset SET size = size + 1;").ok());

    const auto reopened = photobridge::SqliteManifestBuilder::Reopen(
        connection.value(),
        frozen.value().manifest_id);
    ASSERT_FALSE(reopened.ok());
    EXPECT_EQ(reopened.status().code(), photobridge::StatusCode::kInternal);
}

TEST_F(ManifestBuilderTest, ReopenRejectsBuildingManifest)
{
    auto connection = photobridge::SqliteConnection::Open(database_path_);
    ASSERT_TRUE(connection.ok()) << connection.status().message();
    ASSERT_TRUE(photobridge::EnsureSchema(connection.value()).ok());

    photobridge::SqliteManifestBuilder builder(connection.value());
    ASSERT_TRUE(builder.Begin({"source-1", "local-folder", "/photos"}).ok());

    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            connection.value().native_handle(),
            "SELECT manifest_id FROM source_manifest LIMIT 1;",
            -1,
            &statement,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    const std::string manifest_id(
        reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
    sqlite3_finalize(statement);

    const auto reopened = photobridge::SqliteManifestBuilder::Reopen(
        connection.value(),
        manifest_id);
    ASSERT_FALSE(reopened.ok());
    EXPECT_EQ(reopened.status().code(), photobridge::StatusCode::kInvalidArgument);
}

}  // namespace
