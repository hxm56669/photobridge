#include "photobridge/app/fd_permit_pool.h"

namespace photobridge {

FdPermitPool::FdPermitPool(std::size_t capacity) noexcept
    : capacity_(capacity)
{
}

Status FdPermitPool::Acquire()
{
    if (capacity_ == 0) {
        return Status(
            StatusCode::kInvalidArgument,
            "file descriptor permit pool capacity must be positive");
    }
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] {
        return closed_ || in_use_ < capacity_;
    });
    if (closed_) {
        return Status(
            StatusCode::kInvalidArgument,
            "file descriptor permit pool is closed");
    }
    ++in_use_;
    return Status::Ok();
}

void FdPermitPool::Release() noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (in_use_ != 0) --in_use_;
    }
    condition_.notify_all();
}

void FdPermitPool::Close() noexcept
{
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    condition_.notify_all();
}

std::size_t FdPermitPool::capacity() const noexcept
{
    return capacity_;
}

std::size_t FdPermitPool::in_use() const noexcept
{
    std::lock_guard lock(mutex_);
    return in_use_;
}

}  // namespace photobridge
