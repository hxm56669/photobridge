#include <string>

#include <gtest/gtest.h>

#include "photobridge/lan/serve_bootstrap.h"

TEST(ServeBootstrapTest, GeneratesTokenAndUrlForExplicitBind)
{
    auto bootstrap = photobridge::ServeBootstrap::Create(
        photobridge::ServeOptions{"192.168.1.20", 9090});

    ASSERT_TRUE(bootstrap.ok()) << bootstrap.status().message();
    EXPECT_EQ(bootstrap.value().bind_address, "192.168.1.20");
    EXPECT_EQ(bootstrap.value().port, 9090);
    EXPECT_EQ(bootstrap.value().token.size(), 43U);
    EXPECT_EQ(
        bootstrap.value().url,
        "http://192.168.1.20:9090/?t=" + bootstrap.value().token);
    EXPECT_FALSE(bootstrap.value().qr_code.empty());
}

TEST(ServeBootstrapTest, FormatsIpv6UrlWithBrackets)
{
    auto bootstrap = photobridge::ServeBootstrap::Create(
        photobridge::ServeOptions{"fe80::1", 8787});

    ASSERT_TRUE(bootstrap.ok()) << bootstrap.status().message();
    EXPECT_EQ(
        bootstrap.value().url,
        "http://[fe80::1]:8787/?t=" + bootstrap.value().token);
}

TEST(ServeBootstrapTest, RejectsPortZero)
{
    auto bootstrap = photobridge::ServeBootstrap::Create(
        photobridge::ServeOptions{"127.0.0.1", 0});

    ASSERT_FALSE(bootstrap.ok());
    EXPECT_EQ(
        bootstrap.status().code(),
        photobridge::StatusCode::kInvalidArgument);
}
