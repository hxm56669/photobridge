#pragma once

#include <cstddef>
#include <vector>

#include "photobridge/common/status_or.h"
#include "photobridge/model/association_edge.h"
#include "photobridge/model/logical_asset.h"

namespace photobridge {

class AssociationGraph final {
public:
    Status AddVertex(PhysicalAssetId id);
    Status AddEdge(AssociationEdge edge);

    StatusOr<std::vector<LogicalAsset>> BuildLogicalAssets() const;

    std::size_t vertex_count() const noexcept;
    std::size_t edge_count() const noexcept;

private:
    std::vector<PhysicalAssetId> vertices_;
    std::vector<AssociationEdge> edges_;
};

}  // namespace photobridge
