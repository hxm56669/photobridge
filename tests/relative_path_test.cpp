#include <string>

#include <gtest/gtest.h>

#include "photobridge/model/relative_path.h"

TEST(RelativePathTest, ParsesComponentsAndPreservesBytes)
{
    const auto result = photobridge::RelativePath::Parse(
        "album/photo.jpg");

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().bytes(), "album/photo.jpg");
    ASSERT_EQ(result.value().components().size(), 2U);
    EXPECT_EQ(result.value().components()[0], "album");
    EXPECT_EQ(result.value().components()[1], "photo.jpg");
}

TEST(RelativePathTest, RejectsUnsafeComponents)
{
    const std::string invalid_paths[] = {
        "",
        "/absolute/path",
        "album//photo.jpg",
        "./photo.jpg",
        "album/../photo.jpg",
        "album/photo.jpg/",
        std::string("album/") + '\0' + "photo.jpg",
    };

    for (const auto& path : invalid_paths) {
        EXPECT_FALSE(
            photobridge::RelativePath::Parse(path).ok())
            << path;
    }
}

TEST(RelativePathTest, EscapesNonPrintableBytesForDisplay)
{
    const auto result = photobridge::RelativePath::Parse(
        std::string("photo") + '\x01' + ".jpg");

    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().DisplayString(), "photo\\x01.jpg");
}
