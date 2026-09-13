#include "photobridge/model/audit_report.h"

#include <nlohmann/json.hpp>

namespace photobridge {
namespace {

const char* DiffKindName(DiffKind kind)
{
    switch (kind) {
    case DiffKind::kAdded: return "added";
    case DiffKind::kRemoved: return "removed";
    case DiffKind::kChanged: return "changed";
    }
    return "unknown";
}

}  // namespace

std::string RenderAuditReport(const AuditReport& report)
{
    nlohmann::json diff = nlohmann::json::array();
    for (const DiffEntry& entry : report.diff) {
        diff.push_back({
            {"path", entry.path},
            {"kind", DiffKindName(entry.kind)},
        });
    }
    return nlohmann::json{
        {"plan_id", report.plan_id},
        {"task_id", report.task_id},
        {"action", report.action},
        {"verification", report.verification},
        {"diff", diff},
        {"error", report.error},
    }.dump() + "\n";
}

}  // namespace photobridge
