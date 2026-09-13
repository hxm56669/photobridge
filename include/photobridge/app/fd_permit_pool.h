#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>

#include "photobridge/common/status.h"

namespace photobridge {

class FdPermitPool final {
public:
    explicit FdPermitPool(std::size_t capacity) noexcept;

    Status Acquire();
    void Release() noexcept;
    void Close() noexcept;

    std::size_t capacity() const noexcept;
    std::size_t in_use() const noexcept;

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t in_use_ = 0;
    bool closed_ = false;
};

}  // namespace photobridge
