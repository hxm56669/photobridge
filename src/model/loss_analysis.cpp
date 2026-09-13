#include "photobridge/model/loss_analysis.h"

namespace photobridge {

StatusOr<LossAnalysis> AnalyzeCapabilityLoss(
    const TargetCapabilities& capabilities)
{
    const Status validation = capabilities.Validate();
    if (!validation.ok()) return validation;

    LossAnalysis result;
    result.capability_version = capabilities.version;
    for (const Capability& capability : capabilities.entries) {
        if (capability.level == SupportLevel::kFull) continue;
        result.losses.push_back(CapabilityLoss{
            capability.kind,
            capability.level,
            capability.representation,
            capability.reason,
        });
    }
    return result;
}

}  // namespace photobridge
