#include <iostream>

#include "photobridge/common/logging.h"

#include "photobridge/cli/cli_app.h"

int main(int argc, char* argv[]) {
    photobridge::InitLogging();

    return photobridge::RunCli(
        argc,
        argv,
        std::cout,
        std::cerr);
}
