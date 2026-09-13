#pragma once

#include <array>
#include <cstddef>
#include <compare>
#include <string>
#include <string_view>

#include "photobridge/common/status_or.h"

namespace photobridge {

struct Digest {
    std::array<std::byte, 32> bytes{};

    std::string ToHex() const;
    static StatusOr<Digest> FromHex(std::string_view hex);

    auto operator<=>(const Digest&) const = default;
};

}  // namespace photobridge
