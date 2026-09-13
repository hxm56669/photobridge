#pragma once

#include <string_view>

#include "photobridge/common/status.h"
#include "photobridge/model/physical_asset.h"

namespace photobridge {

class PhysicalAssetSink {
public:
    virtual Status Add(PhysicalAsset asset) = 0;
    virtual ~PhysicalAssetSink() = default;
};

class SourceAdapter {
public:
    virtual Status Scan(PhysicalAssetSink& sink) = 0;
    virtual std::string_view TypeName() const noexcept = 0;
    virtual ~SourceAdapter() = default;
};

}  // namespace photobridge
