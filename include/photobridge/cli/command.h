#pragma once

#include <ostream>

#include "photobridge/common/status.h"

namespace photobridge {

class WorkspaceService;

struct CommandContext {
    std::ostream& out;
    std::ostream& err;
    WorkspaceService& workspace_service;
};

class Command {
public:
    virtual ~Command() = default;

    virtual Status Execute(CommandContext& context) = 0;
};

}  // namespace photobridge
