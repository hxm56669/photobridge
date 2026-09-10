#pragma once

#include <string_view>

#include "photobridge/common/status.h"

namespace photobridge {

Status StatusFromErrno(
    int error_number,
    std::string_view operation);

}  // namespace photobridge
