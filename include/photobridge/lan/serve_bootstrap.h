#pragma once

#include <cstdint>
#include <string>

#include "photobridge/common/status_or.h"

namespace photobridge {

struct ServeOptions {
    std::string bind_address;
    std::uint16_t port = 8787;
};

struct ServeBootstrap {
    std::string bind_address;
    std::uint16_t port = 0;
    std::string token;
    std::string url;
    std::string qr_code;

    static StatusOr<ServeBootstrap> Create(ServeOptions options);
};

}  // namespace photobridge
