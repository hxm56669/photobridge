#pragma once

#include <string>
#include <string_view>

#include "photobridge/common/digest.h"
#include "photobridge/common/status_or.h"
#include "photobridge/model/canonical_plan.h"

namespace photobridge {

inline constexpr std::string_view kCanonicalPlanSemanticVersion =
    "canonical-plan-semantic-v1";

struct FrozenPlanFile {
    std::string plan_id;
    std::string bytes;
    Digest artifact_digest;
    Digest semantic_digest;
    CanonicalMinimalPlan plan;
};

// Serializes a typed frozen plan into the versioned JSON Lines artifact and
// computes the exact-byte artifact digest plus the semantic projection digest.
StatusOr<FrozenPlanFile> WriteFrozenPlan(
    const CanonicalMinimalPlan& plan,
    std::string plan_id);

// Parses and validates a JSON Lines artifact, then recomputes both digests
// from the supplied bytes and the parsed typed plan.
StatusOr<FrozenPlanFile> ReadFrozenPlan(std::string_view bytes);

Digest ArtifactDigestFor(std::string_view bytes);
Digest SemanticDigestFor(const CanonicalMinimalPlan& plan);

}  // namespace photobridge
