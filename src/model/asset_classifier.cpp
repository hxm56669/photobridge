#include "photobridge/model/asset_classifier.h"

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace photobridge {
namespace {

std::string LowerAscii(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const unsigned char byte : value) {
        result.push_back(static_cast<char>(std::tolower(byte)));
    }
    return result;
}

std::string ExtensionOf(std::string_view path)
{
    const std::size_t separator = path.rfind('/');
    const std::string_view name = separator == std::string_view::npos
        ? path
        : path.substr(separator + 1);
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) {
        return {};
    }
    return LowerAscii(name.substr(dot + 1));
}

bool IsMediaExtension(std::string_view extension)
{
    constexpr std::string_view kMediaExtensions[] = {
        "arw", "avi", "bmp", "cr2", "dng", "gif", "heic", "heif",
        "jpeg", "jpg", "m4v", "mkv", "mov", "mp4", "nef", "png",
        "raw", "tif", "tiff", "webm", "webp",
    };

    for (const auto candidate : kMediaExtensions) {
        if (extension == candidate) {
            return true;
        }
    }
    return false;
}

AssetKind ClassifyKind(
    std::string_view path,
    std::string_view extension)
{
    const std::size_t separator = path.rfind('/');
    const std::string_view name = separator == std::string_view::npos
        ? path
        : path.substr(separator + 1);
    const std::string lower_name = LowerAscii(name);

    if (lower_name == "metadata.json") {
        return AssetKind::kAlbumMetadata;
    }
    if (extension == "json") {
        return AssetKind::kSidecarJson;
    }
    if (extension == "xmp") {
        return AssetKind::kSidecarXmp;
    }
    if (IsMediaExtension(extension)) {
        return AssetKind::kMedia;
    }
    return AssetKind::kUnknown;
}

}  // namespace

std::optional<PhysicalAsset> ClassifyPhysicalAsset(
    const DirectoryEntry& entry)
{
    if (entry.kind != DirectoryEntryKind::kRegularFile) {
        return std::nullopt;
    }

    const std::string extension = ExtensionOf(entry.relative_path.bytes());
    return PhysicalAsset{
        entry.relative_path,
        entry.identity,
        ClassifyKind(entry.relative_path.bytes(), extension),
        extension,
    };
}

}  // namespace photobridge
