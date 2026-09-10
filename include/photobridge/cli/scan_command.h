#pragma once

#include "photobridge/cli/command.h"

namespace photobridge {

class ScanCommand : public Command {

public:

    Status Execute(CommandContext& context) override;

};

} // namespace photobridge
