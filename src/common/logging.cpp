#include "photobridge/common/logging.h"

#include <spdlog/spdlog.h>

namespace photobridge {

void InitLogging() {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%t] [%l] %v");
}

}  // namespace photobridge