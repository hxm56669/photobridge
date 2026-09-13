#pragma once

#include <optional>

#include "photobridge/model/directory_entry.h"
#include "photobridge/model/physical_asset.h"

namespace photobridge {

std::optional<PhysicalAsset> ClassifyPhysicalAsset(
    const DirectoryEntry& entry);

}  // namespace photobridge
