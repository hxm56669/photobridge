#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/model/diff_engine.h"

TEST(DiffEngineTest, ProducesSortedAddedRemovedAndChangedEntries)
{
    photobridge::Digest old_digest;
    old_digest.bytes[0] = std::byte{1};
    photobridge::Digest new_digest;
    new_digest.bytes[0] = std::byte{2};
    const std::vector<photobridge::DiffFileState> expected{
        {"b.jpg", 2, old_digest},
        {"c.jpg", 3, old_digest},
    };
    const std::vector<photobridge::DiffFileState> observed{
        {"d.jpg", 4, new_digest},
        {"b.jpg", 2, new_digest},
    };
    const auto diff = photobridge::DiffFileStates(expected, observed);
    ASSERT_EQ(diff.size(), 3U);
    EXPECT_EQ(diff[0].path, "b.jpg");
    EXPECT_EQ(diff[0].kind, photobridge::DiffKind::kChanged);
    EXPECT_EQ(diff[1].path, "c.jpg");
    EXPECT_EQ(diff[1].kind, photobridge::DiffKind::kRemoved);
    EXPECT_EQ(diff[2].path, "d.jpg");
    EXPECT_EQ(diff[2].kind, photobridge::DiffKind::kAdded);
}
