#pragma once

#include <string>
#include <vector>

#include "photobridge/model/diff_engine.h"

namespace photobridge {

struct AuditReport {
    std::string plan_id;
    std::string task_id;
    std::string action;
    std::string verification;
    std::vector<DiffEntry> diff;
    std::string error;
};

// Stable one-record JSONL rendering for recovery/audit output.
std::string RenderAuditReport(const AuditReport& report);

}  // namespace photobridge
