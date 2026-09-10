#pragma once

#include <ostream>

#include "photobridge/common/status.h"

namespace photobridge {

int RunCli(
    int argc,
    char* argv[],
    std::ostream& out,
    std::ostream& err);

int ExitCodeForStatus(const Status& status) noexcept;

}  // namespace photobridge
