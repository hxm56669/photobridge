#pragma once

#include <string>
#include <string_view>

#include "photobridge/model/file_identity.h"
#include "photobridge/model/relative_path.h"

namespace photobridge {

using PhysicalAssetId = std::string;

enum class AssetKind {
    kMedia,
    kSidecarJson,
    kSidecarXmp,
    kAlbumMetadata,
    kUnknown,
};

struct PhysicalAsset {
    RelativePath relative_path;
    FileIdentity identity;
    AssetKind kind = AssetKind::kUnknown;
    std::string extension;
};

// The L0 identity is scoped to a source manifest and is derived solely from
// the canonical relative path bytes. File metadata is intentionally excluded:
// a changed file is a mutation of the same manifest member, not a new member.
PhysicalAssetId PhysicalAssetIdFor(const PhysicalAsset& asset);

}  // namespace photobridge
