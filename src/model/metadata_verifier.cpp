#include "photobridge/model/metadata_verifier.h"

namespace photobridge {

MetadataVerificationResult VerifyMetadata(
    const std::optional<MetadataValue>& expected,
    const std::optional<MetadataValue>& actual)
{
    if (!expected.has_value() || !actual.has_value()) {
        return {
            MetadataVerification::kUnverifiable,
            "expected or actual metadata is missing",
        };
    }
    if (MetadataValuesEqual(expected.value(), actual.value())) {
        return {
            MetadataVerification::kIdentical,
            "metadata values match",
        };
    }
    return {
        MetadataVerification::kMismatch,
        "metadata values differ",
    };
}

}  // namespace photobridge
