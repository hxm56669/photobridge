#pragma once

#include <optional>
#include <span>
#include <string>

#include "photobridge/model/photo_ir.h"

namespace photobridge {

enum class RelationVerification {
    kIdentical,
    kMismatch,
    kUnverifiable,
};

struct RelationVerificationResult {
    RelationVerification status = RelationVerification::kUnverifiable;
    std::string reason;
};

RelationVerificationResult VerifyRelations(
    const std::optional<std::span<const PhotoRelationship>>& expected,
    const std::optional<std::span<const PhotoRelationship>>& actual);

}  // namespace photobridge
