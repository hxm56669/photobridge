#include "photobridge/model/physical_asset.h"

#include <cstddef>

namespace photobridge {
namespace {

std::string HexEncode(std::string_view value)
{
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const unsigned char byte : value) {
        result.push_back(kHex[byte >> 4]);
        result.push_back(kHex[byte & 0x0F]);
    }
    return result;
}

}  // namespace

PhysicalAssetId PhysicalAssetIdFor(const PhysicalAsset& asset)
{
    return "path-" + HexEncode(asset.relative_path.bytes());
}

}  // namespace photobridge
