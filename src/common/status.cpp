#include "photobridge/common/status.h"

#include <utility>

namespace photobridge {

Status::Status(StatusCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

Status Status::Ok() {
    return Status(StatusCode::kOk, "");
}

bool Status::ok() const {
    return code_ == StatusCode::kOk;
}

StatusCode Status::code() const {
    return code_;
}

const std::string& Status::message() const {
    return message_;
}

}  // namespace photobridge