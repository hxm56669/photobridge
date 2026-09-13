#pragma once

#include <string>
#include <vector>

#include "photobridge/common/status_or.h"
#include "photobridge/model/capability.h"

namespace photobridge {

struct CapabilityLoss {
    CapabilityKind kind;
    SupportLevel level;
    std::string representation;
    std::string reason;
};

struct LossAnalysis {
    std::string capability_version;
    std::vector<CapabilityLoss> losses;
};

// Reports every capability that is not fully supported. Reporting is pure and
// does not transform or reject an otherwise valid migration plan.
StatusOr<LossAnalysis> AnalyzeCapabilityLoss(
    const TargetCapabilities& capabilities);

}  // namespace photobridge
