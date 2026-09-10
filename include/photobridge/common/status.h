#pragma once

#include <string>

namespace photobridge {

enum class StatusCode {
    kOk = 0,
    kInvalidArgument,
    kNotFound,
    kAlreadyExists,
    kPermissionDenied,
    kIoError,
    kInternal,
};

class Status {
public:
    Status(StatusCode code, std::string message);

    static Status Ok();

    bool ok() const;
    StatusCode code() const;
    const std::string& message() const;

private:
    StatusCode code_;
    std::string message_;
};

}  // namespace photobridge
