#include <optional>
#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "photobridge/model/asset_classifier.h"

namespace {

photobridge::DirectoryEntry Entry(
    std::string path,
    photobridge::DirectoryEntryKind kind)
{
    auto relative_path = photobridge::RelativePath::Parse(std::move(path));
    EXPECT_TRUE(relative_path.ok());
    return photobridge::DirectoryEntry{
        std::move(relative_path.value()),
        photobridge::FileIdentity{},
        kind,
    };
}

}  // namespace

TEST(AssetClassifierTest, ClassifiesKnownExtensionsCaseInsensitively)
{
    const auto media = photobridge::ClassifyPhysicalAsset(
        Entry("album/Photo.JPG", photobridge::DirectoryEntryKind::kRegularFile));
    const auto json = photobridge::ClassifyPhysicalAsset(
        Entry("album/Photo.JSON", photobridge::DirectoryEntryKind::kRegularFile));
    const auto xmp = photobridge::ClassifyPhysicalAsset(
        Entry("album/Photo.XMP", photobridge::DirectoryEntryKind::kRegularFile));

    ASSERT_TRUE(media.has_value());
    EXPECT_EQ(media->kind, photobridge::AssetKind::kMedia);
    EXPECT_EQ(media->extension, "jpg");
    ASSERT_TRUE(json.has_value());
    EXPECT_EQ(json->kind, photobridge::AssetKind::kSidecarJson);
    ASSERT_TRUE(xmp.has_value());
    EXPECT_EQ(xmp->kind, photobridge::AssetKind::kSidecarXmp);
}

TEST(AssetClassifierTest, RecognizesAlbumMetadataByStableName)
{
    const auto result = photobridge::ClassifyPhysicalAsset(
        Entry("takeout/Metadata.JSON", photobridge::DirectoryEntryKind::kRegularFile));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->kind, photobridge::AssetKind::kAlbumMetadata);
    EXPECT_EQ(result->extension, "json");
}

TEST(AssetClassifierTest, PreservesUnknownRegularFiles)
{
    const auto result = photobridge::ClassifyPhysicalAsset(
        Entry("notes.txt", photobridge::DirectoryEntryKind::kRegularFile));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->kind, photobridge::AssetKind::kUnknown);
    EXPECT_EQ(result->extension, "txt");
}

TEST(AssetClassifierTest, PreservesPathAndFileIdentity)
{
    auto entry = Entry(
        "album/photo.jpg",
        photobridge::DirectoryEntryKind::kRegularFile);
    entry.identity.device = 17;
    entry.identity.inode = 42;
    entry.identity.size = 1234;
    entry.identity.mtime_ns = 5678;
    entry.identity.ctime_ns = 9012;

    const auto result = photobridge::ClassifyPhysicalAsset(entry);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->relative_path.bytes(), "album/photo.jpg");
    EXPECT_EQ(result->identity.device, 17U);
    EXPECT_EQ(result->identity.inode, 42U);
    EXPECT_EQ(result->identity.size, 1234U);
    EXPECT_EQ(result->identity.mtime_ns, 5678);
    EXPECT_EQ(result->identity.ctime_ns, 9012);
}

TEST(AssetClassifierTest, SkipsNonRegularEntries)
{
    const auto directory = photobridge::ClassifyPhysicalAsset(
        Entry("album", photobridge::DirectoryEntryKind::kDirectory));
    const auto symlink = photobridge::ClassifyPhysicalAsset(
        Entry("link.jpg", photobridge::DirectoryEntryKind::kSymlink));

    EXPECT_FALSE(directory.has_value());
    EXPECT_FALSE(symlink.has_value());
}
