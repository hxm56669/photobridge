#include <algorithm>

#include <gtest/gtest.h>

#include "photobridge/model/capability.h"

TEST(CapabilityTest, LocalDirectoryV1HasStableValidatedContract)
{
    const auto first = photobridge::LocalDirectoryCapabilitiesV1();
    const auto second = photobridge::LocalDirectoryCapabilitiesV1();

    EXPECT_TRUE(first.Validate().ok());
    EXPECT_EQ(first.version, "directory-v1");
    ASSERT_EQ(first.entries.size(), 8U);
    ASSERT_EQ(second.entries.size(), first.entries.size());
    for (std::size_t index = 0; index < first.entries.size(); ++index) {
        EXPECT_EQ(first.entries[index].kind, second.entries[index].kind);
        EXPECT_EQ(first.entries[index].level, second.entries[index].level);
        EXPECT_EQ(
            first.entries[index].representation,
            second.entries[index].representation);
        EXPECT_EQ(first.entries[index].reason, second.entries[index].reason);
        if (index != 0) {
            EXPECT_LT(
                static_cast<int>(first.entries[index - 1].kind),
                static_cast<int>(first.entries[index].kind));
        }
    }
}

TEST(CapabilityTest, ExposesL0SupportLevelsExplicitly)
{
    const auto capabilities =
        photobridge::LocalDirectoryCapabilitiesV1();

    const auto* bytes = capabilities.Find(
        photobridge::CapabilityKind::kByteExactContents);
    const auto* symlinks = capabilities.Find(
        photobridge::CapabilityKind::kSymlinks);
    const auto* case_folding = capabilities.Find(
        photobridge::CapabilityKind::kCaseFoldSemantics);

    ASSERT_NE(bytes, nullptr);
    ASSERT_NE(symlinks, nullptr);
    ASSERT_NE(case_folding, nullptr);
    EXPECT_EQ(bytes->level, photobridge::SupportLevel::kFull);
    EXPECT_EQ(symlinks->level, photobridge::SupportLevel::kUnsupported);
    EXPECT_EQ(
        case_folding->level,
        photobridge::SupportLevel::kUnverifiable);
}

TEST(CapabilityTest, RejectsDuplicateOrUnorderedEntries)
{
    auto duplicate = photobridge::LocalDirectoryCapabilitiesV1();
    duplicate.entries.push_back(duplicate.entries.front());
    EXPECT_FALSE(duplicate.Validate().ok());

    auto unordered = photobridge::LocalDirectoryCapabilitiesV1();
    std::swap(unordered.entries[0], unordered.entries[1]);
    EXPECT_FALSE(unordered.Validate().ok());
}

TEST(CapabilityTest, FindReturnsNullForAbsentKind)
{
    const auto capabilities =
        photobridge::LocalDirectoryCapabilitiesV1();

    EXPECT_EQ(
        capabilities.Find(static_cast<photobridge::CapabilityKind>(99)),
        nullptr);
}

TEST(CapabilityTest, SupportLevelsHaveStableNamesAndRejectUnknownValues)
{
    EXPECT_TRUE(photobridge::IsKnownSupportLevel(
        photobridge::SupportLevel::kFull));
    EXPECT_EQ(
        std::string(photobridge::SupportLevelName(
            photobridge::SupportLevel::kManifestOnly)),
        "manifest-only");
    auto capabilities = photobridge::LocalDirectoryCapabilitiesV1();
    capabilities.entries[0].level =
        static_cast<photobridge::SupportLevel>(99);
    EXPECT_EQ(
        capabilities.Validate().code(),
        photobridge::StatusCode::kInvalidArgument);
}

TEST(CapabilityTest, RequireIsThePlannerCapabilityBoundary)
{
    const auto capabilities = photobridge::LocalDirectoryCapabilitiesV1();
    EXPECT_TRUE(capabilities.Require(
        photobridge::CapabilityKind::kRegularFiles,
        photobridge::SupportLevel::kFull).ok());
    EXPECT_EQ(
        capabilities.Require(
            photobridge::CapabilityKind::kSymlinks,
            photobridge::SupportLevel::kFull).code(),
        photobridge::StatusCode::kInvalidArgument);
}
