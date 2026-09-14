#include "photobridge/common/time.h"

#include <chrono>

namespace photobridge {

std::int64_t CurrentTimeNanoseconds()
{
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return now.count();
}

}  // namespace photobridge
