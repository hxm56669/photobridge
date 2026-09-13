#include "photobridge/common/digest.h"

#include <cstddef>
#include <cstdint>

namespace photobridge {
namespace {

constexpr char kHex[] = "0123456789abcdef";

int HexValue(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

}  // namespace

std::string Digest::ToHex() const
{
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const std::byte value : bytes) {
        const auto byte = std::to_integer<std::uint8_t>(value);
        result.push_back(kHex[byte >> 4]);
        result.push_back(kHex[byte & 0x0F]);
    }
    return result;
}

StatusOr<Digest> Digest::FromHex(std::string_view hex)
{
    if (hex.size() != 64) {
        return Status(
            StatusCode::kInvalidArgument,
            "digest hex must contain exactly 64 characters");
    }

    Digest result;
    for (std::size_t index = 0; index < result.bytes.size(); ++index) {
        const int high = HexValue(hex[index * 2]);
        const int low = HexValue(hex[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return Status(
                StatusCode::kInvalidArgument,
                "digest hex contains a non-hexadecimal character");
        }
        result.bytes[index] = static_cast<std::byte>(
            static_cast<std::uint8_t>((high << 4) | low));
    }
    return result;
}

}  // namespace photobridge
