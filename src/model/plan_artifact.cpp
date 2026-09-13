#include "photobridge/model/plan_artifact.h"

#include <blake3.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace photobridge {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxJsonLineBytes = 1U << 20;
constexpr std::string_view kSemanticDomain = "PB_PLAN_SEMANTIC_V1";
constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(std::string_view value)
{
    std::string result;
    result.reserve(((value.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < value.size(); index += 3) {
        const std::size_t remaining = value.size() - index;
        const std::uint32_t first = static_cast<unsigned char>(value[index]);
        const std::uint32_t second = remaining > 1
            ? static_cast<unsigned char>(value[index + 1])
            : 0;
        const std::uint32_t third = remaining > 2
            ? static_cast<unsigned char>(value[index + 2])
            : 0;
        const std::uint32_t combined =
            (first << 16) | (second << 8) | third;
        result.push_back(kBase64Alphabet[(combined >> 18) & 0x3F]);
        result.push_back(kBase64Alphabet[(combined >> 12) & 0x3F]);
        result.push_back(remaining > 1
            ? kBase64Alphabet[(combined >> 6) & 0x3F]
            : '=');
        result.push_back(remaining > 2
            ? kBase64Alphabet[combined & 0x3F]
            : '=');
    }
    return result;
}

int Base64Value(char value)
{
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '+') {
        return 62;
    }
    if (value == '/') {
        return 63;
    }
    return -1;
}

StatusOr<std::string> Base64Decode(std::string_view value)
{
    if (value.size() % 4 != 0) {
        return Status(
            StatusCode::kInvalidArgument,
            "base64 value length must be a multiple of four");
    }

    std::string result;
    result.reserve((value.size() / 4) * 3);
    for (std::size_t index = 0; index < value.size(); index += 4) {
        const bool third_padding = value[index + 2] == '=';
        const bool fourth_padding = value[index + 3] == '=';
        if (value[index] == '=' || value[index + 1] == '='
            || (third_padding && !fourth_padding)) {
            return Status(
                StatusCode::kInvalidArgument,
                "base64 value has invalid padding");
        }
        const int first = Base64Value(value[index]);
        const int second = Base64Value(value[index + 1]);
        const int third = third_padding ? 0 : Base64Value(value[index + 2]);
        const int fourth = fourth_padding ? 0 : Base64Value(value[index + 3]);
        if (first < 0 || second < 0 || third < 0 || fourth < 0) {
            return Status(
                StatusCode::kInvalidArgument,
                "base64 value contains an invalid character");
        }
        if (third_padding && index + 4 != value.size()) {
            return Status(
                StatusCode::kInvalidArgument,
                "base64 padding must occur only at the end");
        }
        if (fourth_padding && index + 4 != value.size()) {
            return Status(
                StatusCode::kInvalidArgument,
                "base64 padding must occur only at the end");
        }

        const std::uint32_t combined =
            (static_cast<std::uint32_t>(first) << 18)
            | (static_cast<std::uint32_t>(second) << 12)
            | (static_cast<std::uint32_t>(third) << 6)
            | static_cast<std::uint32_t>(fourth);
        result.push_back(static_cast<char>((combined >> 16) & 0xFF));
        if (!third_padding) {
            result.push_back(static_cast<char>((combined >> 8) & 0xFF));
        }
        if (!fourth_padding) {
            result.push_back(static_cast<char>(combined & 0xFF));
        }
    }
    return result;
}

StatusOr<std::string> RequiredString(
    const Json& object,
    std::string_view key)
{
    const auto iterator = object.find(key);
    if (iterator == object.end() || !iterator->is_string()) {
        return Status(
            StatusCode::kInvalidArgument,
            "plan JSON field must be a string: " + std::string(key));
    }
    return iterator->get<std::string>();
}

StatusOr<std::uint64_t> RequiredUint64(
    const Json& object,
    std::string_view key)
{
    auto string = RequiredString(object, key);
    if (!string.ok() || string.value().empty()) {
        return string.ok()
            ? Status(
                StatusCode::kInvalidArgument,
                "plan JSON integer string must not be empty: "
                    + std::string(key))
            : string.status();
    }
    std::uint64_t value = 0;
    for (const unsigned char digit : string.value()) {
        if (digit < '0' || digit > '9') {
            return Status(
                StatusCode::kInvalidArgument,
                "plan JSON integer string is invalid: " + std::string(key));
        }
        const std::uint64_t next = value * 10U + (digit - '0');
        if (next < value) {
            return Status(
                StatusCode::kInvalidArgument,
                "plan JSON integer string overflows: " + std::string(key));
        }
        value = next;
    }
    return value;
}

Status CheckRecord(const Json& object, std::string_view record)
{
    if (!object.is_object()) {
        return Status(
            StatusCode::kInvalidArgument,
            "plan JSON line must be an object");
    }
    auto actual = RequiredString(object, "record");
    if (!actual.ok()) {
        return actual.status();
    }
    if (actual.value() != record) {
        return Status(
            StatusCode::kInvalidArgument,
            "unexpected plan JSON record type");
    }
    return Status::Ok();
}

Json IdentityJson(const FileIdentity& identity)
{
    Json result{
        {"ctime_ns", std::to_string(identity.ctime_ns)},
        {"device", std::to_string(identity.device)},
        {"inode", std::to_string(identity.inode)},
        {"mtime_ns", std::to_string(identity.mtime_ns)},
        {"size", std::to_string(identity.size)},
    };
    if (identity.mount_id.has_value()) {
        result["mount_id"] = std::to_string(identity.mount_id.value());
    } else {
        result["mount_id"] = nullptr;
    }
    return result;
}

StatusOr<FileIdentity> ParseIdentity(const Json& object)
{
    if (!object.is_object()) {
        return Status(
            StatusCode::kInvalidArgument,
            "plan source_identity must be an object");
    }
    FileIdentity result;
    auto device = RequiredUint64(object, "device");
    auto inode = RequiredUint64(object, "inode");
    auto size = RequiredUint64(object, "size");
    auto mtime = RequiredString(object, "mtime_ns");
    auto ctime = RequiredString(object, "ctime_ns");
    if (!device.ok()) {
        return device.status();
    }
    if (!inode.ok()) {
        return inode.status();
    }
    if (!size.ok()) {
        return size.status();
    }
    if (!mtime.ok() || !ctime.ok()) {
        return !mtime.ok() ? mtime.status() : ctime.status();
    }

    auto ParseSigned = [](const std::string& value) -> StatusOr<std::int64_t> {
        if (value.empty()) {
            return Status(
                StatusCode::kInvalidArgument,
                "plan signed integer string must not be empty");
        }
        bool negative = value.front() == '-';
        const std::size_t start = negative ? 1 : 0;
        if (start == value.size()) {
            return Status(
                StatusCode::kInvalidArgument,
                "plan signed integer string is invalid");
        }
        std::uint64_t magnitude = 0;
        for (std::size_t index = start; index < value.size(); ++index) {
            const unsigned char digit = value[index];
            if (digit < '0' || digit > '9') {
                return Status(
                    StatusCode::kInvalidArgument,
                    "plan signed integer string is invalid");
            }
            const std::uint64_t next = magnitude * 10U + (digit - '0');
            if (next < magnitude) {
                return Status(
                    StatusCode::kInvalidArgument,
                    "plan signed integer string overflows");
            }
            magnitude = next;
        }
        constexpr std::uint64_t kInt64MinMagnitude =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            + 1U;
        if ((!negative
             && magnitude > static_cast<std::uint64_t>(
                 std::numeric_limits<std::int64_t>::max()))
            || (negative && magnitude > kInt64MinMagnitude)) {
            return Status(
                StatusCode::kInvalidArgument,
                "plan signed integer string overflows");
        }
        if (negative) {
            if (magnitude == kInt64MinMagnitude) {
                return std::numeric_limits<std::int64_t>::min();
            }
            return -static_cast<std::int64_t>(magnitude);
        }
        return static_cast<std::int64_t>(magnitude);
    };

    auto mtime_value = ParseSigned(mtime.value());
    auto ctime_value = ParseSigned(ctime.value());
    if (!mtime_value.ok()) {
        return mtime_value.status();
    }
    if (!ctime_value.ok()) {
        return ctime_value.status();
    }
    result.device = device.value();
    result.inode = inode.value();
    result.size = size.value();
    result.mtime_ns = mtime_value.value();
    result.ctime_ns = ctime_value.value();

    const auto mount = object.find("mount_id");
    if (mount == object.end()) {
        return Status(
            StatusCode::kInvalidArgument,
            "plan source_identity is missing mount_id");
    }
    if (!mount->is_null()) {
        if (!mount->is_string()) {
            return Status(
                StatusCode::kInvalidArgument,
                "plan mount_id must be a string or null");
        }
        auto mount_value = RequiredUint64(object, "mount_id");
        if (!mount_value.ok()) {
            return mount_value.status();
        }
        result.mount_id = mount_value.value();
    }
    return result;
}

Json CapabilityJson(const Capability& capability)
{
    return Json{
        {"kind", std::to_string(static_cast<int>(capability.kind))},
        {"level", std::to_string(static_cast<int>(capability.level))},
        {"reason", capability.reason},
        {"representation", capability.representation},
    };
}

StatusOr<Capability> ParseCapability(const Json& object)
{
    if (!object.is_object()) {
        return Status(
            StatusCode::kInvalidArgument,
            "plan capability must be an object");
    }
    auto kind = RequiredUint64(object, "kind");
    auto level = RequiredUint64(object, "level");
    auto representation = RequiredString(object, "representation");
    auto reason = RequiredString(object, "reason");
    if (!kind.ok()) {
        return kind.status();
    }
    if (!level.ok()) {
        return level.status();
    }
    if (!representation.ok()) {
        return representation.status();
    }
    if (!reason.ok()) {
        return reason.status();
    }
    if (kind.value() > 7 || level.value() > 5) {
        return Status(
            StatusCode::kInvalidArgument,
            "plan capability enum value is out of range");
    }
    return Capability{
        static_cast<CapabilityKind>(kind.value()),
        static_cast<SupportLevel>(level.value()),
        std::move(representation.value()),
        std::move(reason.value()),
    };
}

Json HeaderJson(const CanonicalMinimalPlan& plan, std::string_view plan_id)
{
    Json policy{
        {"max_component_bytes", std::to_string(plan.policy().max_component_bytes)},
        {"max_path_bytes", std::to_string(plan.policy().max_path_bytes)},
        {"preserve_source_directories", plan.policy().preserve_source_directories},
        {"target_prefix_b64", Base64Encode(plan.policy().target_prefix)},
    };
    Json capabilities = Json::array();
    for (const Capability& capability : plan.capabilities().entries) {
        capabilities.push_back(CapabilityJson(capability));
    }
    return Json{
        {"capabilities", std::move(capabilities)},
        {"format_version", std::to_string(plan.format_version())},
        {"plan_id", plan_id},
        {"policy", std::move(policy)},
        {"record", "header"},
        {"semantic_version", std::string(kCanonicalPlanSemanticVersion)},
        {"capability_version", plan.capabilities().version},
        {"source_manifest_digest", plan.source_manifest_digest().ToHex()},
        {"source_manifest_id", plan.source_manifest_id()},
        {"target_root_b64", Base64Encode(plan.target_root())},
    };
}

Json AssetJson(const MinimalPlanAsset& asset)
{
    return Json{
        {"logical_asset_id", asset.logical_asset_id},
        {"record", "asset"},
        {"source_asset_id", asset.source_asset_id},
        {"source_identity", IdentityJson(asset.source_identity)},
        {"source_path_b64", Base64Encode(asset.source_path.bytes())},
        {"target_path_b64", Base64Encode(asset.target_path.bytes())},
    };
}

std::string SectionLine(std::string_view section)
{
    return Json{{"record", "section"}, {"section", section}}.dump();
}

std::string Serialize(
    const CanonicalMinimalPlan& plan,
    std::string_view plan_id)
{
    std::string result;
    result += HeaderJson(plan, plan_id).dump();
    result.push_back('\n');
    for (const MinimalPlanAsset& asset : plan.assets()) {
        result += AssetJson(asset).dump();
        result.push_back('\n');
    }
    for (const std::string_view section : {
             std::string_view{"outputs"},
             std::string_view{"tasks"},
             std::string_view{"dependencies"},
             std::string_view{"losses"}}) {
        result += SectionLine(section);
        result.push_back('\n');
    }
    result += Json{{"record", "end"}}.dump();
    result.push_back('\n');
    return result;
}

StatusOr<CanonicalMinimalPlan> Parse(
    std::string_view bytes,
    std::string& plan_id)
{
    if (bytes.empty() || bytes.back() != '\n') {
        return Status(
            StatusCode::kInvalidArgument,
            "plan JSON Lines artifact must end with LF");
    }

    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start < bytes.size()) {
        const std::size_t newline = bytes.find('\n', start);
        if (newline == std::string_view::npos) {
            break;
        }
        if (newline == start || newline - start > kMaxJsonLineBytes
            || bytes[start] == '\r'
            || (newline > start && bytes[newline - 1] == '\r')) {
            return Status(
                StatusCode::kInvalidArgument,
                "plan JSON Lines contains an empty, oversized, or CRLF line");
        }
        lines.push_back(bytes.substr(start, newline - start));
        start = newline + 1;
    }
    if (lines.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "plan JSON Lines artifact has no records");
    }

    std::vector<Json> records;
    records.reserve(lines.size());
    try {
        for (const std::string_view line : lines) {
            records.push_back(Json::parse(line));
        }
    } catch (const Json::exception& error) {
        return Status(
            StatusCode::kInvalidArgument,
            std::string("plan JSON Lines parse failed: ") + error.what());
    }

    Status status = CheckRecord(records.front(), "header");
    if (!status.ok()) {
        return status;
    }
    auto parsed_plan_id = RequiredString(records.front(), "plan_id");
    auto format_version = RequiredUint64(records.front(), "format_version");
    auto manifest_id = RequiredString(records.front(), "source_manifest_id");
    auto manifest_digest = RequiredString(
        records.front(),
        "source_manifest_digest");
    auto target_root = RequiredString(records.front(), "target_root_b64");
    auto semantic_version = RequiredString(
        records.front(),
        "semantic_version");
    if (!parsed_plan_id.ok()) {
        return parsed_plan_id.status();
    }
    if (!format_version.ok() || format_version.value() != 1) {
        return Status(
            StatusCode::kInvalidArgument,
            "unsupported minimal plan format version");
    }
    if (!manifest_id.ok()) {
        return manifest_id.status();
    }
    if (!manifest_digest.ok()) {
        return manifest_digest.status();
    }
    if (!target_root.ok()) {
        return target_root.status();
    }
    if (!semantic_version.ok()
        || semantic_version.value() != kCanonicalPlanSemanticVersion) {
        return Status(
            StatusCode::kInvalidArgument,
            "unsupported canonical plan semantic version");
    }
    if (parsed_plan_id.value().empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "plan id must not be empty");
    }
    auto decoded_root = Base64Decode(target_root.value());
    auto digest = Digest::FromHex(manifest_digest.value());
    if (!decoded_root.ok()) {
        return decoded_root.status();
    }
    if (!digest.ok()) {
        return digest.status();
    }

    const auto policy_iterator = records.front().find("policy");
    const auto capabilities_iterator = records.front().find("capabilities");
    if (policy_iterator == records.front().end()
        || capabilities_iterator == records.front().end()) {
        return Status(
            StatusCode::kInvalidArgument,
            "plan header is missing policy or capabilities");
    }
    const Json& policy_json = *policy_iterator;
    const Json& capability_json = *capabilities_iterator;
    if (!policy_json.is_object() || !capability_json.is_array()) {
        return Status(
            StatusCode::kInvalidArgument,
            "plan policy or capabilities has the wrong type");
    }
    auto target_prefix = RequiredString(policy_json, "target_prefix_b64");
    auto preserve = policy_json.find("preserve_source_directories");
    auto max_component = RequiredUint64(policy_json, "max_component_bytes");
    auto max_path = RequiredUint64(policy_json, "max_path_bytes");
    if (!target_prefix.ok()) {
        return target_prefix.status();
    }
    if (preserve == policy_json.end() || !preserve->is_boolean()) {
        return Status(
            StatusCode::kInvalidArgument,
            "plan preserve_source_directories must be boolean");
    }
    if (!max_component.ok()) {
        return max_component.status();
    }
    if (!max_path.ok()) {
        return max_path.status();
    }
    auto decoded_prefix = Base64Decode(target_prefix.value());
    if (!decoded_prefix.ok()) {
        return decoded_prefix.status();
    }

    TargetCapabilities capabilities;
    auto capability_version = RequiredString(records.front(), "capability_version");
    if (!capability_version.ok()) {
        return capability_version.status();
    }
    capabilities.version = std::move(capability_version.value());
    for (const Json& capability_value : capability_json) {
        auto capability = ParseCapability(capability_value);
        if (!capability.ok()) {
            return capability.status();
        }
        capabilities.entries.push_back(std::move(capability.value()));
    }

    MinimalPlanInput input;
    input.source_manifest_id = std::move(manifest_id.value());
    input.source_manifest_digest = digest.value();
    input.target_root = std::move(decoded_root.value());
    input.capabilities = std::move(capabilities);
    input.policy = MigrationPolicy{
        std::move(decoded_prefix.value()),
        preserve->get<bool>(),
        static_cast<std::size_t>(max_component.value()),
        static_cast<std::size_t>(max_path.value()),
    };

    std::size_t index = 1;
    while (index < records.size()) {
        const Json& record = records[index];
        auto record_type = RequiredString(record, "record");
        if (!record_type.ok()) {
            return record_type.status();
        }
        if (record_type.value() == "asset") {
            if (!record.is_object()) {
                return Status(
                    StatusCode::kInvalidArgument,
                    "plan asset record must be an object");
            }
            auto logical_id = RequiredString(record, "logical_asset_id");
            auto source_id = RequiredString(record, "source_asset_id");
            auto source_path = RequiredString(record, "source_path_b64");
            auto target_path = RequiredString(record, "target_path_b64");
            if (!logical_id.ok()) {
                return logical_id.status();
            }
            if (!source_id.ok()) {
                return source_id.status();
            }
            if (!source_path.ok()) {
                return source_path.status();
            }
            if (!target_path.ok()) {
                return target_path.status();
            }
            auto decoded_source = Base64Decode(source_path.value());
            auto decoded_target = Base64Decode(target_path.value());
            if (!decoded_source.ok()) {
                return decoded_source.status();
            }
            if (!decoded_target.ok()) {
                return decoded_target.status();
            }
            auto parsed_source_path = RelativePath::Parse(
                std::move(decoded_source.value()));
            auto parsed_target_path = RelativePath::Parse(
                std::move(decoded_target.value()));
            if (!parsed_source_path.ok()) {
                return parsed_source_path.status();
            }
            if (!parsed_target_path.ok()) {
                return parsed_target_path.status();
            }
            const auto identity = record.find("source_identity");
            if (identity == record.end()) {
                return Status(
                    StatusCode::kInvalidArgument,
                    "plan asset is missing source_identity");
            }
            auto parsed_identity = ParseIdentity(*identity);
            if (!parsed_identity.ok()) {
                return parsed_identity.status();
            }
            input.assets.push_back(MinimalPlanAsset{
                std::move(logical_id.value()),
                std::move(source_id.value()),
                std::move(parsed_source_path.value()),
                std::move(parsed_target_path.value()),
                parsed_identity.value(),
            });
            ++index;
            continue;
        }
        break;
    }

    constexpr std::string_view kSections[] = {
        "outputs", "tasks", "dependencies", "losses"};
    for (const std::string_view section : kSections) {
        if (index >= records.size()) {
            return Status(
                StatusCode::kInvalidArgument,
                "plan is missing a fixed section");
        }
        status = CheckRecord(records[index], "section");
        if (!status.ok()) {
            return status;
        }
        auto actual_section = RequiredString(records[index], "section");
        if (!actual_section.ok()) {
            return actual_section.status();
        }
        if (actual_section.value() != section) {
            return Status(
                StatusCode::kInvalidArgument,
                "plan sections are out of order");
        }
        ++index;
    }
    if (index + 1 != records.size()) {
        return Status(
            StatusCode::kInvalidArgument,
            "plan has unexpected records after fixed sections");
    }
    status = CheckRecord(records[index], "end");
    if (!status.ok()) {
        return status;
    }

    plan_id = std::move(parsed_plan_id.value());
    return CanonicalMinimalPlan::Build(std::move(input));
}

Digest Blake3Digest(std::string_view domain, std::string_view payload)
{
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, domain.data(), domain.size());
    blake3_hasher_update(&hasher, payload.data(), payload.size());
    Digest digest;
    blake3_hasher_finalize(
        &hasher,
        reinterpret_cast<std::uint8_t*>(digest.bytes.data()),
        digest.bytes.size());
    return digest;
}

}  // namespace

Digest ArtifactDigestFor(std::string_view bytes)
{
    // artifact_digest is intentionally the BLAKE3 of the exact published
    // bytes. The semantic digest has its own domain separation below.
    return Blake3Digest({}, bytes);
}

Digest SemanticDigestFor(const CanonicalMinimalPlan& plan)
{
    return Blake3Digest(kSemanticDomain, plan.CanonicalPlanPayload());
}

StatusOr<FrozenPlanFile> WriteFrozenPlan(
    const CanonicalMinimalPlan& plan,
    std::string plan_id)
{
    if (plan_id.empty() || plan_id.find('\0') != std::string::npos) {
        return Status(
            StatusCode::kInvalidArgument,
            "plan id must be non-empty and must not contain NUL");
    }
    std::string bytes = Serialize(plan, plan_id);
    return FrozenPlanFile{
        std::move(plan_id),
        bytes,
        ArtifactDigestFor(bytes),
        SemanticDigestFor(plan),
        plan,
    };
}

StatusOr<FrozenPlanFile> ReadFrozenPlan(std::string_view bytes)
{
    std::string plan_id;
    auto plan = Parse(bytes, plan_id);
    if (!plan.ok()) {
        return plan.status();
    }
    return FrozenPlanFile{
        std::move(plan_id),
        std::string(bytes),
        ArtifactDigestFor(bytes),
        SemanticDigestFor(plan.value()),
        std::move(plan.value()),
    };
}

}  // namespace photobridge
