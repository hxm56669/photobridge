#pragma once

#include <optional>
#include <string>

#include "photobridge/model/metadata_candidate.h"

namespace photobridge {

enum class MetadataVerification {
    kIdentical,
    kMismatch,
    kUnverifiable,
};

struct MetadataVerificationResult {
    MetadataVerification status = MetadataVerification::kUnverifiable;
    std::string reason;
};

MetadataVerificationResult VerifyMetadata(
    const std::optional<MetadataValue>& expected,
    const std::optional<MetadataValue>& actual);

}  // namespace photobridge
