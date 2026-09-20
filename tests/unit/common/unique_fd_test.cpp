#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include "photobridge/common/unique_fd.h"

namespace {

int OpenDevNull()
{
    return ::open("/dev/null", O_RDONLY);
}

}  // namespace

TEST(UniqueFdTest, DefaultValueIsInvalid)
{
    const photobridge::UniqueFd fd;

    EXPECT_EQ(fd.get(), -1);
    EXPECT_FALSE(static_cast<bool>(fd));
}

TEST(UniqueFdTest, MoveTransfersOwnership)
{
    const int raw_fd = OpenDevNull();
    ASSERT_GE(raw_fd, 0);

    photobridge::UniqueFd source(raw_fd);
    photobridge::UniqueFd moved(std::move(source));

    EXPECT_EQ(source.get(), -1);
    EXPECT_EQ(moved.get(), raw_fd);

    const int released_fd = moved.release();
    EXPECT_EQ(released_fd, raw_fd);
    EXPECT_EQ(moved.get(), -1);
    EXPECT_EQ(::close(released_fd), 0);
}

TEST(UniqueFdTest, ResetClosesPreviousDescriptor)
{
    const int raw_fd = OpenDevNull();
    ASSERT_GE(raw_fd, 0);

    photobridge::UniqueFd fd(raw_fd);
    fd.reset();

    errno = 0;
    EXPECT_EQ(::fcntl(raw_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}
