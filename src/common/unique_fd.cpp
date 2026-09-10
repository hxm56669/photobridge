#include "photobridge/common/unique_fd.h"

#include <unistd.h>

namespace photobridge {

UniqueFd::UniqueFd(int fd) noexcept
    : fd_(fd)
{
}

UniqueFd::~UniqueFd()
{
    reset();
}

UniqueFd::UniqueFd(UniqueFd&& other) noexcept
    : fd_(other.release())
{
}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept
{
    if (this != &other) {
        reset(other.release());
    }

    return *this;
}

int UniqueFd::get() const noexcept
{
    return fd_;
}

UniqueFd::operator bool() const noexcept
{
    return fd_ >= 0;
}

int UniqueFd::release() noexcept
{
    const int released_fd = fd_;
    fd_ = -1;
    return released_fd;
}

void UniqueFd::reset(int fd) noexcept
{
    if (fd_ >= 0) {
        ::close(fd_);
    }

    fd_ = fd;
}

}  // namespace photobridge
