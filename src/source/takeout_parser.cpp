#include "photobridge/source/takeout_parser.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "photobridge/filesystem/linux_directory_walker.h"
#include "photobridge/filesystem/linux_file_ops.h"
#include "photobridge/model/asset_classifier.h"
#include "photobridge/model/metadata_rules.h"

namespace photobridge {
namespace {

constexpr std::uint64_t kMaximumJsonBytes = 8U * 1024U * 1024U;
constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;

class TakeoutEntrySink final : public DirectoryEntrySink {
public:
    Status Add(DirectoryEntry entry) override
    {
        entries_.push_back(std::move(entry));
        return Status::Ok();
    }

    const std::vector<DirectoryEntry>& entries() const noexcept
    {
        return entries_;
    }

private:
    std::vector<DirectoryEntry> entries_;
};

void AddError(
    TakeoutParseResult& result,
    const RelativePath& path,
    std::string code,
    std::string message)
{
    result.errors.push_back(TakeoutParserError{
        path,
        std::move(code),
        std::move(message),
    });
}

StatusOr<std::string> ReadBounded(
    LinuxFileOps& file_ops,
    int fd,
    std::uint64_t expected_size)
{
    if (expected_size > kMaximumJsonBytes) {
        return Status(
            StatusCode::kInvalidArgument,
            "JSON sidecar exceeds the 8 MiB parser limit");
    }

    const std::size_t capacity = static_cast<std::size_t>(expected_size);
    std::vector<std::byte> bytes(capacity + 1U);
    std::size_t total = 0;
    while (total < bytes.size()) {
        auto read = file_ops.Read(
            fd,
            std::span<std::byte>(bytes).subspan(total));
        if (!read.ok()) {
            return read.status();
        }
        if (read.value() == 0) {
            break;
        }
        total += read.value();
    }

    if (total == bytes.size()) {
        return Status(
            StatusCode::kInvalidArgument,
            "JSON sidecar exceeds the 8 MiB parser limit");
    }
    if (total != capacity) {
        return Status(
            StatusCode::kIoError,
            "JSON sidecar changed while it was being read");
    }

    return std::string(
        reinterpret_cast<const char*>(bytes.data()),
        total);
}

bool IsJsonAsset(AssetKind kind)
{
    return kind == AssetKind::kSidecarJson
        || kind == AssetKind::kAlbumMetadata;
}

bool IsSidecar(AssetKind kind)
{
    return kind == AssetKind::kSidecarJson
        || kind == AssetKind::kSidecarXmp;
}

std::string LowerAscii(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const unsigned char byte : value) {
        result.push_back(static_cast<char>(std::tolower(byte)));
    }
    return result;
}

std::string BasenameWithoutExtension(const PhysicalAsset& asset)
{
    const std::string_view name = asset.relative_path.components().back();
    const std::size_t dot = name.rfind('.');
    const std::string_view stem = dot == std::string_view::npos
        || dot == 0
        ? name
        : name.substr(0, dot);
    return LowerAscii(stem);
}

bool IsKnownMediaExtension(std::string_view extension)
{
    constexpr std::string_view kExtensions[] = {
        "arw", "avi", "bmp", "cr2", "dng", "gif", "heic", "heif",
        "jpeg", "jpg", "m4v", "mkv", "mov", "mp4", "nef", "png",
        "raw", "tif", "tiff", "webm", "webp",
    };
    for (const auto candidate : kExtensions) {
        if (extension == candidate) {
            return true;
        }
    }
    return false;
}

std::string CanonicalSidecarStem(const PhysicalAsset& asset)
{
    std::string stem = BasenameWithoutExtension(asset);
    const std::size_t dot = stem.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == stem.size()) {
        return stem;
    }
    if (IsKnownMediaExtension(stem.substr(dot + 1))) {
        stem.erase(dot);
    }
    return stem;
}

bool UsesMediaExtensionSidecarName(const PhysicalAsset& asset)
{
    const std::string stem = BasenameWithoutExtension(asset);
    const std::size_t dot = stem.rfind('.');
    return dot != std::string::npos
        && dot != 0
        && dot + 1 < stem.size()
        && IsKnownMediaExtension(stem.substr(dot + 1));
}

std::string BasenameKey(const PhysicalAsset& asset)
{
    std::string key;
    const auto& components = asset.relative_path.components();
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        if (!key.empty()) {
            key.push_back('/');
        }
        key += components[index];
    }
    if (!key.empty()) {
        key.push_back('/');
    }
    key += IsSidecar(asset.kind)
        ? CanonicalSidecarStem(asset)
        : BasenameWithoutExtension(asset);
    return key;
}

void AddRelationErrorsAndCandidates(TakeoutParseResult& result)
{
    using AssetList = std::vector<const PhysicalAsset*>;
    std::unordered_map<std::string, AssetList> media_by_key;
    std::unordered_map<std::string, AssetList> sidecars_by_key;
    for (const PhysicalAsset& asset : result.assets) {
        const std::string key = BasenameKey(asset);
        if (asset.kind == AssetKind::kMedia) {
            media_by_key[key].push_back(&asset);
        } else if (IsSidecar(asset.kind)) {
            sidecars_by_key[key].push_back(&asset);
        }
    }

    std::unordered_set<std::string> media_relation_keys;
    for (const auto& [key, media] : media_by_key) {
        const PhysicalAsset* raw = nullptr;
        const PhysicalAsset* jpeg = nullptr;
        std::size_t raw_count = 0;
        std::size_t jpeg_count = 0;
        for (const PhysicalAsset* asset : media) {
            const bool is_raw = asset->extension == "arw"
                || asset->extension == "cr2"
                || asset->extension == "dng"
                || asset->extension == "nef"
                || asset->extension == "raw";
            const bool is_jpeg = asset->extension == "jpg"
                || asset->extension == "jpeg";
            if (is_raw) {
                raw = asset;
                ++raw_count;
            } else if (is_jpeg) {
                jpeg = asset;
                ++jpeg_count;
            }
        }
        if (raw_count == 1U && jpeg_count == 1U) {
            result.relations.push_back(SourceRelationCandidate{
                PhysicalAssetIdFor(*raw),
                PhysicalAssetIdFor(*jpeg),
                SourceRelationKind::kRawJpegPair,
                "takeout.raw-jpeg.v1",
                jpeg->relative_path.DisplayString()
                    + " pairs "
                    + raw->relative_path.DisplayString(),
            });
            media_relation_keys.insert(key);
        } else if (raw_count != 0U && jpeg_count != 0U) {
            for (const PhysicalAsset* asset : media) {
                AddError(
                    result,
                    asset->relative_path,
                    "ambiguous_raw_jpeg_relation",
                    "same basename has multiple RAW or JPEG media files");
            }
            media_relation_keys.insert(key);
        }

        const PhysicalAsset* still = nullptr;
        const PhysicalAsset* motion = nullptr;
        std::size_t still_count = 0;
        std::size_t motion_count = 0;
        for (const PhysicalAsset* asset : media) {
            const bool is_motion = asset->extension == "mov";
            const bool is_still = asset->extension == "jpg"
                || asset->extension == "jpeg"
                || asset->extension == "heic"
                || asset->extension == "heif";
            if (is_motion) {
                motion = asset;
                ++motion_count;
            } else if (is_still) {
                still = asset;
                ++still_count;
            }
        }
        if (still_count == 1U && motion_count == 1U) {
            result.relations.push_back(SourceRelationCandidate{
                PhysicalAssetIdFor(*still),
                PhysicalAssetIdFor(*motion),
                SourceRelationKind::kLivePhotoMotionForMedia,
                "takeout.live-photo.v1",
                motion->relative_path.DisplayString()
                    + " motion-pairs "
                    + still->relative_path.DisplayString(),
            });
            media_relation_keys.insert(key);
        } else if (still_count != 0U && motion_count != 0U) {
            for (const PhysicalAsset* asset : media) {
                AddError(
                    result,
                    asset->relative_path,
                    "ambiguous_live_photo_relation",
                    "same basename has multiple still or motion media files");
            }
            media_relation_keys.insert(key);
        }
    }

    std::unordered_map<std::string, AssetList> media_by_prefix;
    std::unordered_set<std::string> media_keys_with_weak_sidecar;
    for (const auto& [key, media] : media_by_key) {
        const std::size_t directory_end = key.rfind('/');
        const std::size_t first_stem_byte = directory_end == std::string::npos
            ? 0U
            : directory_end + 1U;
        constexpr std::size_t kMinimumUnambiguousStemBytes = 3U;
        if (key.size() <= first_stem_byte + kMinimumUnambiguousStemBytes) {
            continue;
        }
        for (std::size_t length = first_stem_byte + kMinimumUnambiguousStemBytes;
             length < key.size();
             ++length) {
            media_by_prefix[key.substr(0, length)].insert(
                media_by_prefix[key.substr(0, length)].end(),
                media.begin(),
                media.end());
        }
    }

    for (const auto& [key, sidecars] : sidecars_by_key) {
        const auto media = media_by_key.find(key);
        for (const PhysicalAsset* sidecar : sidecars) {
            if (media == media_by_key.end()) {
                const auto weak_media = media_by_prefix.find(key);
                if (weak_media != media_by_prefix.end()) {
                    const std::string message = weak_media->second.size() == 1U
                        ? "sidecar basename is a strict prefix of a media basename; relation is not inferred"
                        : "sidecar basename is a strict prefix of multiple media basenames; relation is ambiguous";
                    AddError(
                        result,
                        sidecar->relative_path,
                        weak_media->second.size() == 1U
                            ? "truncated_basename_match"
                            : "ambiguous_truncated_basename",
                        message);
                    if (weak_media->second.size() == 1U) {
                        media_keys_with_weak_sidecar.insert(
                            BasenameKey(*weak_media->second.front()));
                    }
                    continue;
                }
                AddError(
                    result,
                    sidecar->relative_path,
                    "orphan_sidecar",
                    "sidecar has no media with the same directory and basename");
                continue;
            }
            if (media->second.size() != 1U || sidecars.size() != 1U) {
                AddError(
                    result,
                    sidecar->relative_path,
                    "ambiguous_sidecar_relation",
                    "same basename matches multiple media or sidecar files");
                continue;
            }

            const PhysicalAsset* media_asset = media->second.front();
            const SourceRelationKind relation = sidecar->kind
                == AssetKind::kSidecarXmp
                ? SourceRelationKind::kXmpSidecarForMedia
                : SourceRelationKind::kJsonSidecarForMedia;
            result.relations.push_back(SourceRelationCandidate{
                PhysicalAssetIdFor(*media_asset),
                PhysicalAssetIdFor(*sidecar),
                relation,
                UsesMediaExtensionSidecarName(*sidecar)
                    ? "takeout.sidecar-media-extension.v1"
                    : "takeout.basename.v1",
                sidecar->relative_path.DisplayString()
                    + " matches "
                    + media_asset->relative_path.DisplayString(),
            });
        }
    }

    for (const auto& [key, media] : media_by_key) {
        if (sidecars_by_key.find(key) != sidecars_by_key.end()
            || media_relation_keys.find(key) != media_relation_keys.end()
            || media_keys_with_weak_sidecar.find(key)
                != media_keys_with_weak_sidecar.end()) {
            continue;
        }
        for (const PhysicalAsset* media_asset : media) {
            AddError(
                result,
                media_asset->relative_path,
                "missing_sidecar",
                "media has no JSON or XMP sidecar with the same directory and basename");
        }
    }
}

void AddCandidate(
    TakeoutParseResult& result,
    const PhysicalAsset& asset,
    MetadataField field,
    MetadataValue value,
    std::string rule,
    std::string evidence)
{
    if (!IsMetadataRuleCompatible(
            GoogleTakeoutMetadataRuleset(),
            field,
            MetadataSource::kGoogleTakeoutJson,
            rule)) {
        AddError(
            result,
            asset.relative_path,
            "metadata_rule",
            "metadata candidate rule is not compatible with its field/source");
        return;
    }
    if (!IsMetadataValueCompatible(field, value)) {
        AddError(
            result,
            asset.relative_path,
            "metadata_field_type",
            "metadata candidate value is incompatible with its field");
        return;
    }
    if (field == MetadataField::kTakenTime) {
        const Status time_status = ValidateTimeCandidate(
            std::get<TimeCandidate>(value));
        if (!time_status.ok()) {
            AddError(
                result,
                asset.relative_path,
                "metadata_field_value",
                time_status.message());
            return;
        }
    }
    result.candidates.push_back(MetadataCandidate{
        PhysicalAssetIdFor(asset),
        field,
        std::move(value),
        MetadataSource::kGoogleTakeoutJson,
        std::move(rule),
        std::move(evidence),
    });
}

void AddFieldTypeError(
    TakeoutParseResult& result,
    const PhysicalAsset& asset,
    std::string_view field,
    std::string_view expected)
{
    AddError(
        result,
        asset.relative_path,
        "metadata_field_type",
        "JSON field " + std::string(field)
            + " must be " + std::string(expected));
}

StatusOr<std::int64_t> UnixSecondsToNanoseconds(std::string_view raw_value)
{
    std::int64_t seconds = 0;
    const auto parsed = std::from_chars(
        raw_value.data(),
        raw_value.data() + raw_value.size(),
        seconds);
    if (parsed.ec != std::errc{}
        || parsed.ptr != raw_value.data() + raw_value.size()) {
        return Status(
            StatusCode::kInvalidArgument,
            "photoTakenTime.timestamp must be a signed integer Unix timestamp");
    }
    if (seconds > std::numeric_limits<std::int64_t>::max()
            / kNanosecondsPerSecond
        || seconds < std::numeric_limits<std::int64_t>::min()
            / kNanosecondsPerSecond) {
        return Status(
            StatusCode::kInvalidArgument,
            "photoTakenTime.timestamp overflows Unix nanoseconds");
    }
    return seconds * kNanosecondsPerSecond;
}

void ExtractMetadataCandidates(
    TakeoutParseResult& result,
    const PhysicalAsset& asset,
    const nlohmann::json& document)
{
    if (!document.is_object()) {
        AddError(
            result,
            asset.relative_path,
            "metadata_shape",
            "JSON sidecar root must be an object");
        return;
    }

    const auto title = document.find("title");
    if (title != document.end()) {
        if (!title->is_string()) {
            AddFieldTypeError(result, asset, "title", "a string");
        } else {
            AddCandidate(
                result,
                asset,
                MetadataField::kTitle,
                title->get<std::string>(),
                "takeout.json.title.v1",
                asset.relative_path.DisplayString() + "#/title");
        }
    }

    const auto description = document.find("description");
    if (description != document.end()) {
        if (!description->is_string()) {
            AddFieldTypeError(result, asset, "description", "a string");
        } else {
            AddCandidate(
                result,
                asset,
                MetadataField::kDescription,
                description->get<std::string>(),
                "takeout.json.description.v1",
                asset.relative_path.DisplayString() + "#/description");
        }
    }

    const auto favorite = document.find("favorited");
    if (favorite != document.end()) {
        if (!favorite->is_boolean()) {
            AddFieldTypeError(result, asset, "favorited", "a boolean");
        } else {
            AddCandidate(
                result,
                asset,
                MetadataField::kFavorite,
                favorite->get<bool>(),
                "takeout.json.favorited.v1",
                asset.relative_path.DisplayString() + "#/favorited");
        }
    }

    const auto taken_time = document.find("photoTakenTime");
    if (taken_time == document.end()) {
        return;
    }
    if (!taken_time->is_object()) {
        AddFieldTypeError(result, asset, "photoTakenTime", "an object");
        return;
    }
    const auto timestamp = taken_time->find("timestamp");
    if (timestamp == taken_time->end()) {
        return;
    }

    std::string raw_timestamp;
    if (timestamp->is_string()) {
        raw_timestamp = timestamp->get<std::string>();
    } else if (timestamp->is_number_integer()) {
        raw_timestamp = std::to_string(timestamp->get<std::int64_t>());
    } else {
        AddFieldTypeError(
            result,
            asset,
            "photoTakenTime.timestamp",
            "an integer or integer string");
        return;
    }

    auto unix_ns = UnixSecondsToNanoseconds(raw_timestamp);
    if (!unix_ns.ok()) {
        AddError(
            result,
            asset.relative_path,
            "metadata_field_value",
            unix_ns.status().message());
        return;
    }
    TimeCandidate value;
    value.value = AbsoluteTime{unix_ns.value()};
    value.precision = TimePrecision::kSecond;
    value.source = MetadataSource::kGoogleTakeoutJson;
    value.raw_value = std::move(raw_timestamp);
    AddCandidate(
        result,
        asset,
        MetadataField::kTakenTime,
        std::move(value),
        "takeout.json.photoTakenTime.timestamp.v1",
        asset.relative_path.DisplayString() + "#/photoTakenTime/timestamp");
}

void SortResult(TakeoutParseResult& result)
{
    std::sort(
        result.assets.begin(),
        result.assets.end(),
        [](const PhysicalAsset& left, const PhysicalAsset& right) {
            return left.relative_path.bytes() < right.relative_path.bytes();
        });
    std::sort(
        result.relations.begin(),
        result.relations.end(),
        [](const SourceRelationCandidate& left, const SourceRelationCandidate& right) {
            if (left.media_asset_id != right.media_asset_id) {
                return left.media_asset_id < right.media_asset_id;
            }
            if (left.sidecar_asset_id != right.sidecar_asset_id) {
                return left.sidecar_asset_id < right.sidecar_asset_id;
            }
            return left.evidence < right.evidence;
        });
    std::sort(
        result.candidates.begin(),
        result.candidates.end(),
        [](const MetadataCandidate& left, const MetadataCandidate& right) {
            if (left.asset_id != right.asset_id) {
                return left.asset_id < right.asset_id;
            }
            if (left.field != right.field) {
                return left.field < right.field;
            }
            if (left.extraction_rule != right.extraction_rule) {
                return left.extraction_rule < right.extraction_rule;
            }
            return left.evidence < right.evidence;
        });
    std::sort(
        result.errors.begin(),
        result.errors.end(),
        [](const TakeoutParserError& left, const TakeoutParserError& right) {
            if (left.relative_path.bytes() != right.relative_path.bytes()) {
                return left.relative_path.bytes()
                    < right.relative_path.bytes();
            }
            if (left.code != right.code) {
                return left.code < right.code;
            }
            return left.message < right.message;
        });
}

}  // namespace

StatusOr<TakeoutParseResult> TakeoutParser::Parse(
    const std::filesystem::path& root) const
{
    LinuxFileOps file_ops;
    auto root_fd = file_ops.OpenRoot(root, OpenRootMode::kExisting);
    if (!root_fd.ok()) {
        return root_fd.status();
    }

    TakeoutEntrySink sink;
    LinuxDirectoryWalker walker;
    const Status walk_status = walker.Walk(root_fd.value().get(), sink);
    if (!walk_status.ok()) {
        return walk_status;
    }

    TakeoutParseResult result;
    for (const DirectoryEntry& entry : sink.entries()) {
        auto asset = ClassifyPhysicalAsset(entry);
        if (!asset.has_value()) {
            continue;
        }
        result.assets.push_back(*asset);

        if (asset->kind == AssetKind::kUnknown) {
            AddError(
                result,
                asset->relative_path,
                "unknown_file_kind",
                "regular file has no supported Takeout media or sidecar type");
            continue;
        }
        if (!IsJsonAsset(asset->kind)) {
            continue;
        }

        auto file_fd = file_ops.OpenSource(
            root_fd.value().get(),
            asset->relative_path);
        if (!file_fd.ok()) {
            AddError(
                result,
                asset->relative_path,
                "unreadable_json",
                file_fd.status().message());
            continue;
        }

        auto json_bytes = ReadBounded(
            file_ops,
            file_fd.value().get(),
            asset->identity.size);
        if (!json_bytes.ok()) {
            AddError(
                result,
                asset->relative_path,
                json_bytes.status().code() == StatusCode::kInvalidArgument
                    ? "json_size_limit"
                    : "unreadable_json",
                json_bytes.status().message());
            continue;
        }

        try {
            const auto parsed_json = nlohmann::json::parse(json_bytes.value());
            ExtractMetadataCandidates(result, *asset, parsed_json);
        } catch (const nlohmann::json::parse_error& error) {
            AddError(
                result,
                asset->relative_path,
                "invalid_json",
                error.what());
        }
    }

    AddRelationErrorsAndCandidates(result);
    SortResult(result);
    return result;
}

}  // namespace photobridge
