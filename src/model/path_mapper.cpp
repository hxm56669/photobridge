#include "photobridge/model/path_mapper.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string_view>
#include <utility>

namespace photobridge {
namespace {

bool IsNonAscii(std::string_view value)
{
    for (const unsigned char byte : value) {
        if (byte >= 0x80) {
            return true;
        }
    }
    return false;
}

std::string AsciiCaseFoldKey(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const unsigned char byte : value) {
        result.push_back(static_cast<char>(
            byte < 0x80
                ? std::tolower(byte)
                : byte));
    }
    return result;
}

StatusOr<RelativePath> BuildTargetPath(
    const RelativePath& source_path,
    const MigrationPolicy& policy)
{
    std::string target;
    if (!policy.target_prefix.empty()) {
        target = policy.target_prefix;
        target.push_back('/');
    }

    if (policy.preserve_source_directories) {
        target.append(source_path.bytes());
    } else {
        target.append(source_path.components().back());
    }
    return RelativePath::Parse(std::move(target));
}

Status ValidateTargetPath(
    const RelativePath& target_path,
    const MigrationPolicy& policy)
{
    if (target_path.bytes().size() > policy.max_path_bytes) {
        return Status(
            StatusCode::kInvalidArgument,
            "mapped target path exceeds the configured byte limit");
    }
    for (const std::string& component : target_path.components()) {
        if (component.size() > policy.max_component_bytes) {
            return Status(
                StatusCode::kInvalidArgument,
                "mapped target path component exceeds the configured byte limit");
        }
    }
    return Status::Ok();
}

}  // namespace

Status MigrationPolicy::Validate() const
{
    if (max_component_bytes == 0 || max_path_bytes == 0) {
        return Status(
            StatusCode::kInvalidArgument,
            "target path limits must be greater than zero");
    }
    if (!target_prefix.empty()) {
        auto parsed_prefix = RelativePath::Parse(target_prefix);
        if (!parsed_prefix.ok()) {
            return parsed_prefix.status();
        }
    }
    return Status::Ok();
}

StatusOr<PathMapping> TargetPathMapper::Map(
    const LogicalAsset& asset,
    const TargetCapabilities& capabilities,
    const MigrationPolicy& policy) const
{
    Status status = policy.Validate();
    if (!status.ok()) {
        return status;
    }
    status = capabilities.Validate();
    if (!status.ok()) {
        return status;
    }
    if (asset.id.empty() || asset.members.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "logical asset identity and members must not be empty");
    }
    if (!asset.source_path.has_value()) {
        return Status(
            StatusCode::kInvalidArgument,
            "L0 logical asset is missing its source path");
    }

    status = capabilities.Require(
        CapabilityKind::kRegularFiles,
        SupportLevel::kFull);
    if (!status.ok()) {
        return status;
    }
    status = capabilities.Require(
        CapabilityKind::kByteExactContents,
        SupportLevel::kFull);
    if (!status.ok()) {
        return status;
    }
    if (policy.preserve_source_directories
        || asset.source_path->components().size() > 1
        || policy.target_prefix.find('/') != std::string::npos) {
        status = capabilities.Require(
            CapabilityKind::kNestedDirectories,
            SupportLevel::kFull);
        if (!status.ok()) {
            return status;
        }
    }

    auto target_path = BuildTargetPath(asset.source_path.value(), policy);
    if (!target_path.ok()) {
        return target_path.status();
    }
    status = ValidateTargetPath(target_path.value(), policy);
    if (!status.ok()) {
        return status;
    }

    const Capability* normalization = capabilities.Find(
        CapabilityKind::kUnicodeNormalization);
    if (IsNonAscii(target_path.value().bytes())
        && (normalization == nullptr
            || normalization->level != SupportLevel::kFull)) {
        return Status(
            StatusCode::kInvalidArgument,
            "Unicode normalization semantics are unverifiable for this target");
    }

    return PathMapping{
        asset.id,
        std::move(target_path.value()),
    };
}

StatusOr<std::vector<PathMapping>> TargetPathMapper::MapAll(
    const std::vector<LogicalAsset>& assets,
    const TargetCapabilities& capabilities,
    const MigrationPolicy& policy) const
{
    std::vector<PathMapping> mappings;
    mappings.reserve(assets.size());
    for (const LogicalAsset& asset : assets) {
        auto mapping = Map(asset, capabilities, policy);
        if (!mapping.ok()) {
            return mapping.status();
        }
        mappings.push_back(std::move(mapping.value()));
    }

    std::sort(
        mappings.begin(),
        mappings.end(),
        [](const PathMapping& lhs, const PathMapping& rhs) {
            return lhs.logical_asset_id < rhs.logical_asset_id;
        });
    for (std::size_t index = 1; index < mappings.size(); ++index) {
        if (mappings[index - 1].logical_asset_id
            == mappings[index].logical_asset_id) {
            return Status(
                StatusCode::kInvalidArgument,
                "logical asset ids must be unique in a path mapping");
        }
    }

    std::map<std::string, LogicalAssetId> exact_paths;
    std::map<std::string, LogicalAssetId> folded_paths;
    for (const PathMapping& mapping : mappings) {
        const std::string exact_key(mapping.target_path.bytes());
        const auto exact_inserted = exact_paths.emplace(
            exact_key,
            mapping.logical_asset_id);
        if (!exact_inserted.second) {
            return Status(
                StatusCode::kAlreadyExists,
                "multiple logical assets map to the same target path");
        }

        const std::string folded_key = AsciiCaseFoldKey(exact_key);
        const auto folded_inserted = folded_paths.emplace(
            folded_key,
            mapping.logical_asset_id);
        if (!folded_inserted.second) {
            return Status(
                StatusCode::kAlreadyExists,
                "multiple logical assets collide under ASCII case folding");
        }
    }
    return mappings;
}

}  // namespace photobridge
