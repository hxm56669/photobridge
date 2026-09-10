#include "photobridge/common/posix_error.h"

#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

namespace photobridge {
namespace {

StatusCode StatusCodeFromErrno(int error_number)
{
    switch (error_number) {
    case 0:
        return StatusCode::kOk;
    case EINVAL:
        return StatusCode::kInvalidArgument;
    case ENOENT:
    case ENOTDIR:
        return StatusCode::kNotFound;
    case EEXIST:
        return StatusCode::kAlreadyExists;
    case EACCES:
    case EPERM:
        return StatusCode::kPermissionDenied;
    case EBADF:
        return StatusCode::kInternal;
    case EINTR:
    case EAGAIN:
    case EIO:
    case ENOSPC:
    case EDQUOT:
    case EROFS:
        return StatusCode::kIoError;
    default:
        return StatusCode::kIoError;
    }
}

}  // namespace

Status StatusFromErrno(
    int error_number,
    std::string_view operation)
{
    const StatusCode code = StatusCodeFromErrno(error_number);
    if (code == StatusCode::kOk) {
        return Status::Ok();
    }

    const std::error_code error(
        error_number,
        std::generic_category());
    std::string message(operation);
    message += ": [";
    message += std::to_string(error_number);
    message += "] ";
    message += error.message();

    return Status(code, std::move(message));
}

}  // namespace photobridge
