#include <gtest/gtest.h>

#include "photobridge/common/status.h"


TEST(StatusTest, OkStatus)
{
    auto status = photobridge::Status::Ok();

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(status.code(), photobridge::StatusCode::kOk);
}

TEST(StatusTest, ErrorStatus)
{
    auto status = photobridge::Status(
        photobridge::StatusCode::kNotFound,
        "file missing"
    );

    EXPECT_FALSE(status.ok());

    EXPECT_EQ(
        status.code(),
        photobridge::StatusCode::kNotFound
    );

    EXPECT_EQ(
        status.message(),
        "file missing"
    );
}