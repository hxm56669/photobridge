#include <cerrno>
#include <string>

#include <gtest/gtest.h>

#include "photobridge/common/posix_error.h"

TEST(PosixErrorTest, MapsExpectedErrnoCategories)
{
    EXPECT_TRUE(
        photobridge::StatusFromErrno(0, "open").ok());
    EXPECT_EQ(
        photobridge::StatusFromErrno(EINVAL, "open").code(),
        photobridge::StatusCode::kInvalidArgument);
    EXPECT_EQ(
        photobridge::StatusFromErrno(ENOENT, "open").code(),
        photobridge::StatusCode::kNotFound);
    EXPECT_EQ(
        photobridge::StatusFromErrno(EEXIST, "mkdir").code(),
        photobridge::StatusCode::kAlreadyExists);
    EXPECT_EQ(
        photobridge::StatusFromErrno(EACCES, "open").code(),
        photobridge::StatusCode::kPermissionDenied);
    EXPECT_EQ(
        photobridge::StatusFromErrno(EBADF, "read").code(),
        photobridge::StatusCode::kInternal);
    EXPECT_EQ(
        photobridge::StatusFromErrno(EIO, "read").code(),
        photobridge::StatusCode::kIoError);
}

TEST(PosixErrorTest, PreservesOperationInErrorMessage)
{
    const auto status = photobridge::StatusFromErrno(
        ENOENT,
        "open source file");

    EXPECT_FALSE(status.ok());
    EXPECT_NE(
        status.message().find("open source file"),
        std::string::npos);
}
