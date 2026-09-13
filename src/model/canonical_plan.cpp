#include "photobridge/model/canonical_plan.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

namespace photobridge {
namespace {

void AppendByte(std::string_view value, std::string& output)
{
    output.append(value.data(), value.size());
}

void AppendUint64(std::uint64_t value, std::string& output)
{
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output.push_back(static_cast<char>((value >> (index * 8)) & 0xFFU));
    }
}

void AppendLengthPrefixed(std::string_view value, std::string& output)
{
    AppendUint64(static_cast<std::uint64_t>(value.size()), output);
    AppendByte(value, output);
}

void AppendBool(bool value, std::string& output)
{
    output.push_back(value ? '\x01' : '\x00');
}

void AppendFileIdentity(const FileIdentity& identity, std::string& output)
{
    AppendUint64(identity.device, output);
    AppendUint64(identity.inode, output);
    AppendUint64(identity.size, output);
    AppendUint64(static_cast<std::uint64_t>(identity.mtime_ns), output);
    AppendUint64(static_cast<std::uint64_t>(identity.ctime_ns), output);
    AppendBool(identity.mount_id.has_value(), output);
    if (identity.mount_id.has_value()) {
        AppendUint64(identity.mount_id.value(), output);
    }
}

Status ValidatePlanInput(const MinimalPlanInput& input)
{
    if (input.source_manifest_id.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "minimal plan source manifest id must not be empty");
    }
    if (input.source_manifest_id.find('\0') != std::string::npos) {
        return Status(
            StatusCode::kInvalidArgument,
            "minimal plan source manifest id must not contain NUL");
    }
    if (input.target_root.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "minimal plan target root must not be empty");
    }
    if (input.target_root.find('\0') != std::string::npos) {
        return Status(
            StatusCode::kInvalidArgument,
            "minimal plan target root must not contain NUL");
    }

    Status status = input.capabilities.Validate();
    if (!status.ok()) {
        return status;
    }
    status = input.policy.Validate();
    if (!status.ok()) {
        return status;
    }

    std::vector<std::string_view> logical_ids;
    std::vector<std::string_view> source_ids;
    logical_ids.reserve(input.assets.size());
    source_ids.reserve(input.assets.size());
    for (const MinimalPlanAsset& asset : input.assets) {
        if (asset.logical_asset_id.empty()
            || asset.source_asset_id.empty()) {
            return Status(
                StatusCode::kInvalidArgument,
                "minimal plan asset ids must not be empty");
        }
        if (asset.source_path.bytes().empty()
            || asset.target_path.bytes().empty()) {
            return Status(
                StatusCode::kInvalidArgument,
                "minimal plan asset paths must not be empty");
        }
        logical_ids.push_back(asset.logical_asset_id);
        source_ids.push_back(asset.source_asset_id);
    }

    std::sort(logical_ids.begin(), logical_ids.end());
    if (std::adjacent_find(logical_ids.begin(), logical_ids.end())
        != logical_ids.end()) {
        return Status(
            StatusCode::kInvalidArgument,
            "minimal plan logical asset ids must be unique");
    }
    std::sort(source_ids.begin(), source_ids.end());
    if (std::adjacent_find(source_ids.begin(), source_ids.end())
        != source_ids.end()) {
        return Status(
            StatusCode::kInvalidArgument,
            "minimal plan source asset ids must be unique");
    }
    return Status::Ok();
}

}  // namespace

CanonicalMinimalPlan::CanonicalMinimalPlan(MinimalPlanInput input)
    : input_(std::move(input))
{
    std::sort(
        input_.assets.begin(),
        input_.assets.end(),
        [](const MinimalPlanAsset& lhs, const MinimalPlanAsset& rhs) {
            if (lhs.logical_asset_id != rhs.logical_asset_id) {
                return lhs.logical_asset_id < rhs.logical_asset_id;
            }
            return lhs.source_asset_id < rhs.source_asset_id;
        });
}

StatusOr<CanonicalMinimalPlan> CanonicalMinimalPlan::Build(
    MinimalPlanInput input)
{
    Status status = ValidatePlanInput(input);
    if (!status.ok()) {
        return status;
    }
    return CanonicalMinimalPlan(std::move(input));
}

std::uint32_t CanonicalMinimalPlan::format_version() const noexcept
{
    return kCanonicalMinimalPlanVersion;
}

const std::string& CanonicalMinimalPlan::source_manifest_id() const noexcept
{
    return input_.source_manifest_id;
}

const Digest& CanonicalMinimalPlan::source_manifest_digest() const noexcept
{
    return input_.source_manifest_digest;
}

const std::string& CanonicalMinimalPlan::target_root() const noexcept
{
    return input_.target_root;
}

const TargetCapabilities& CanonicalMinimalPlan::capabilities() const noexcept
{
    return input_.capabilities;
}

const MigrationPolicy& CanonicalMinimalPlan::policy() const noexcept
{
    return input_.policy;
}

const std::vector<MinimalPlanAsset>& CanonicalMinimalPlan::assets() const noexcept
{
    return input_.assets;
}

std::string CanonicalMinimalPlan::CanonicalPlanPayload() const
{
    std::string payload;
    payload.reserve(256 + input_.assets.size() * 128);
    AppendLengthPrefixed(
        "PB_CANONICAL_MINIMAL_PLAN_V1",
        payload);
    AppendUint64(format_version(), payload);
    AppendLengthPrefixed(input_.source_manifest_id, payload);
    AppendByte(
        std::string_view(
            reinterpret_cast<const char*>(
                input_.source_manifest_digest.bytes.data()),
            input_.source_manifest_digest.bytes.size()),
        payload);
    AppendLengthPrefixed(input_.target_root, payload);
    AppendLengthPrefixed(input_.capabilities.version, payload);
    AppendLengthPrefixed(input_.policy.target_prefix, payload);
    AppendBool(input_.policy.preserve_source_directories, payload);
    AppendUint64(input_.policy.max_component_bytes, payload);
    AppendUint64(input_.policy.max_path_bytes, payload);

    AppendUint64(input_.capabilities.entries.size(), payload);
    for (const Capability& capability : input_.capabilities.entries) {
        AppendUint64(static_cast<std::uint64_t>(capability.kind), payload);
        AppendUint64(static_cast<std::uint64_t>(capability.level), payload);
        AppendLengthPrefixed(capability.representation, payload);
        AppendLengthPrefixed(capability.reason, payload);
    }

    AppendUint64(input_.assets.size(), payload);
    for (const MinimalPlanAsset& asset : input_.assets) {
        AppendLengthPrefixed(asset.logical_asset_id, payload);
        AppendLengthPrefixed(asset.source_asset_id, payload);
        AppendLengthPrefixed(asset.source_path.bytes(), payload);
        AppendLengthPrefixed(asset.target_path.bytes(), payload);
        AppendFileIdentity(asset.source_identity, payload);
    }
    return payload;
}

std::string CanonicalMinimalPlan::CanonicalPayload() const
{
    return CanonicalPlanPayload();
}

}  // namespace photobridge
