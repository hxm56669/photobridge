#include "photobridge/app/byte_permit_pool.h"

namespace photobridge {

BytePermitPool::BytePermitPool(std::size_t capacity) noexcept
    : capacity_(capacity)
{
}

Status BytePermitPool::Acquire(std::size_t bytes)
{
    if (bytes > capacity_) {
        return Status(
            StatusCode::kInvalidArgument,
            "byte permit request exceeds pool capacity");
    }
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this, bytes] {
        return closed_ || capacity_ - in_use_ >= bytes;
    });
    if (closed_) {
        return Status(
            StatusCode::kInvalidArgument,
            "byte permit pool is closed");
    }
    in_use_ += bytes;
    return Status::Ok();
}

void BytePermitPool::Release(std::size_t bytes) noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (bytes > in_use_) {
            in_use_ = 0;
        } else {
            in_use_ -= bytes;
        }
    }
    condition_.notify_all();
}

void BytePermitPool::Close() noexcept
{
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    condition_.notify_all();
}

std::size_t BytePermitPool::capacity() const noexcept
{
    return capacity_;
}

std::size_t BytePermitPool::in_use() const noexcept
{
    std::lock_guard lock(mutex_);
    return in_use_;
}

}  // namespace photobridge
