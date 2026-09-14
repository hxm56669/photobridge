#include "photobridge/app/plan_execution_lock.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <utility>

#include "photobridge/common/posix_error.h"

namespace photobridge {

PlanExecutionLock::PlanExecutionLock(UniqueFd fd) noexcept
    : fd_(std::move(fd))
{
}

StatusOr<PlanExecutionLock> PlanExecutionLock::Acquire(
    const std::filesystem::path& lock_path)
{
    const int fd = ::open(
        lock_path.c_str(),
        O_CREAT | O_RDWR | O_CLOEXEC,
        0600);
    if (fd < 0) {
        return StatusFromErrno(errno, "open plan execution lock");
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int error_number = errno;
        ::close(fd);
        if (error_number == EWOULDBLOCK || error_number == EAGAIN) {
            return Status(
                StatusCode::kAlreadyExists,
                "plan already has an active executor: " + lock_path.string());
        }
        return StatusFromErrno(error_number, "acquire plan execution lock");
    }
    return PlanExecutionLock(UniqueFd(fd));
}

}  // namespace photobridge
