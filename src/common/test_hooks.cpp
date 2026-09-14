#include "photobridge/common/test_hooks.h"

#include <chrono>
#include <cstdlib>
#include <thread>

namespace photobridge {

void PauseForTest(const char* environment_name)
{
    const char* value = std::getenv(environment_name);
    if (value == nullptr || *value == '\0') return;
    char* end = nullptr;
    const unsigned long milliseconds = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || milliseconds == 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

}  // namespace photobridge
