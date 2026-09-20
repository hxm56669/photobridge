#include <gtest/gtest.h>

#include "photobridge/common/status_or.h"

TEST(StatusOrTest, SuccessContainsValue)
{
    photobridge::StatusOr<int> result(42);

    EXPECT_TRUE(result.ok());

    EXPECT_EQ(
        result.value(),
        42
    );

    EXPECT_TRUE(
        result.status().ok()
    );
}

TEST(StatusOrTest, ErrorContainsStatus)
{
    photobridge::StatusOr<int> result(
        photobridge::Status(
            photobridge::StatusCode::kNotFound,
            "file missing"
        )
    );

    EXPECT_FALSE(result.ok());

    EXPECT_EQ(
        result.status().code(),
        photobridge::StatusCode::kNotFound
    );

    EXPECT_EQ(
        result.status().message(),
        "file missing"
    );
}