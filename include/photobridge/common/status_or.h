#pragma once

#include "photobridge/common/status.h"

#include <optional>
#include <utility>
#include <cassert>

namespace photobridge {

template <typename T>
class StatusOr {
public:
    StatusOr(T value);
    StatusOr(Status status);

    bool ok() const;
    const Status& status() const;

    T& value();
    const T& value() const;

private:
    std::optional<T> value_;
    Status status_;
};

template <typename T>
StatusOr<T>::StatusOr(T value)
    : value_(std::move(value)),
      status_(Status::Ok()) {}

template <typename T>
StatusOr<T>::StatusOr(Status status)
    : value_(std::nullopt),
      status_(std::move(status)) {
    if (status_.ok()) {
        status_ = Status(
            StatusCode::kInternal,
            "StatusOr cannot hold OK status without a value"
        );
    }
}

template <typename T>
bool StatusOr<T>::ok() const {
    return status_.ok();
}

template <typename T>
const Status& StatusOr<T>::status() const {
    return status_;
}

template <typename T>
T& StatusOr<T>::value() {
    assert(ok());
    return *value_;
}

template <typename T>
const T& StatusOr<T>::value() const {
    assert(ok());
    return *value_;
}

}  // namespace photobridge