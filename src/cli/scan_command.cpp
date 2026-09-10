#include "photobridge/cli/scan_command.h"

namespace photobridge {

Status ScanCommand::Execute(CommandContext& context)
{
    context.out << "scan command\n";

    return Status::Ok();
}

} // namespace photobridge
