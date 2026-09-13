#include "photobridge/model/logical_asset.h"

#include <blake3.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace photobridge {
namespace {

constexpr std::string_view kLogicalAssetDomain = "PB_LOGICAL_ASSET_V1";

void AppendJsonString(std::string_view value, std::string& output)
{
    output.push_back('"');
    constexpr char kHex[] = "0123456789abcdef";
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (byte < 0x20) {
                output += "\\u00";
                output.push_back(kHex[byte >> 4]);
                output.push_back(kHex[byte & 0x0F]);
            } else {
                output.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    output.push_back('"');
}

std::string CanonicalMembersJson(
    const std::vector<PhysicalAssetId>& members)
{
    std::string result;
    result.push_back('[');
    for (std::size_t index = 0; index < members.size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        AppendJsonString(members[index], result);
    }
    result.push_back(']');
    return result;
}

}  // namespace

StatusOr<LogicalAssetId> LogicalAssetIdForMembers(
    std::vector<PhysicalAssetId> members)
{
    if (members.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "logical asset must contain at least one physical asset");
    }

    for (const auto& member : members) {
        if (member.empty()) {
            return Status(
                StatusCode::kInvalidArgument,
                "logical asset member id must not be empty");
        }
    }

    std::sort(members.begin(), members.end());
    if (std::adjacent_find(members.begin(), members.end()) != members.end()) {
        return Status(
            StatusCode::kInvalidArgument,
            "logical asset member ids must be unique");
    }

    const std::string canonical_members = CanonicalMembersJson(members);
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(
        &hasher,
        kLogicalAssetDomain.data(),
        kLogicalAssetDomain.size());
    blake3_hasher_update(
        &hasher,
        canonical_members.data(),
        canonical_members.size());

    std::array<std::byte, 32> digest{};
    blake3_hasher_finalize(
        &hasher,
        reinterpret_cast<std::uint8_t*>(digest.data()),
        digest.size());

    constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(7 + digest.size() * 2);
    encoded = "logical-";
    for (const std::byte value : digest) {
        const auto byte = std::to_integer<std::uint8_t>(value);
        encoded.push_back(kHex[byte >> 4]);
        encoded.push_back(kHex[byte & 0x0F]);
    }
    return encoded;
}

StatusOr<LogicalAsset> MapPhysicalAssetToLogicalAsset(
    const PhysicalAsset& asset)
{
    const PhysicalAssetId physical_id = PhysicalAssetIdFor(asset);
    auto logical_id = LogicalAssetIdForMembers({physical_id});
    if (!logical_id.ok()) {
        return logical_id.status();
    }
    return LogicalAsset{
        std::move(logical_id.value()),
        std::vector<PhysicalAssetId>{physical_id},
        asset.relative_path,
    };
}

}  // namespace photobridge
