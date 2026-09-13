#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "photobridge/common/status_or.h"
#include "photobridge/model/metadata_candidate.h"
#include "photobridge/model/physical_asset.h"
#include "photobridge/model/source_relation_candidate.h"

namespace photobridge {

struct TakeoutParserError {
    RelativePath relative_path;
    std::string code;
    std::string message;
};

struct TakeoutParseResult {
    std::vector<PhysicalAsset> assets;
    std::vector<MetadataCandidate> candidates;
    std::vector<SourceRelationCandidate> relations;
    std::vector<TakeoutParserError> errors;
};

class TakeoutParser final {
public:
    StatusOr<TakeoutParseResult> Parse(
        const std::filesystem::path& root) const;
};

}  // namespace photobridge
