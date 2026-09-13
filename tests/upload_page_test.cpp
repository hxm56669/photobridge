#include <gtest/gtest.h>

#include "photobridge/lan/upload_page.h"

TEST(UploadPageTest, ReturnsSelfContainedHtmlResponse)
{
    const auto response = photobridge::GetUploadPageResponse();

    EXPECT_EQ(response.content_type, "text/html; charset=utf-8");
    EXPECT_NE(response.body.find("<input id=\"files\" type=\"file\" multiple>"),
              std::string::npos);
    EXPECT_NE(response.body.find("<script>"), std::string::npos);
    EXPECT_EQ(response.body.find("http://"), std::string::npos);
    EXPECT_EQ(response.body.find("https://"), std::string::npos);
    EXPECT_EQ(response.body.find("<link"), std::string::npos);
    EXPECT_EQ(response.body.find("<img"), std::string::npos);
}

TEST(UploadPageTest, ContainsNoThirdPartyResourceReference)
{
    const auto response = photobridge::GetUploadPageResponse();

    EXPECT_EQ(response.body.find("cdn"), std::string::npos);
    EXPECT_EQ(response.body.find("src="), std::string::npos);
    EXPECT_EQ(response.body.find("href="), std::string::npos);
}

TEST(UploadPageTest, ComparesTokensWithoutAcceptingLengthMismatch)
{
    EXPECT_TRUE(
        photobridge::ConstantTimeTokenEquals("secret", "secret"));
    EXPECT_FALSE(
        photobridge::ConstantTimeTokenEquals("secret", "secreT"));
    EXPECT_FALSE(
        photobridge::ConstantTimeTokenEquals("secret", "secret-extra"));
}

TEST(UploadPageTest, IncludesThreeLaneMixedSizeScheduler)
{
    const auto response = photobridge::GetUploadPageResponse();

    EXPECT_NE(response.body.find("largeThreshold"), std::string::npos);
    EXPECT_NE(response.body.find("Promise.all([worker(0), worker(1), worker(2)])"),
              std::string::npos);
    EXPECT_NE(response.body.find("activeLarge < 2"), std::string::npos);
}

TEST(UploadPageTest, KeepsThreeWorkerConcurrencyBound)
{
    const auto response = photobridge::GetUploadPageResponse();

    EXPECT_NE(response.body.find("worker(0), worker(1), worker(2)"),
              std::string::npos);
}

TEST(UploadPageTest, IncludesMobileViewportAndAccessibleStatus)
{
    const auto response = photobridge::GetUploadPageResponse();

    EXPECT_NE(response.body.find("name=\"viewport\""), std::string::npos);
    EXPECT_NE(response.body.find("role=\"status\""), std::string::npos);
    EXPECT_NE(response.body.find("button.disabled = true"), std::string::npos);
    EXPECT_NE(response.body.find("Upload failed:"), std::string::npos);
}
