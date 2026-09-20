#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "photobridge/model/audit_report.h"

TEST(AuditReportTest, RendersStableJsonlWithVisibleDiffAndError)
{
    const photobridge::AuditReport report{
        "plan-1",
        "task-1",
        "TARGET_CONFLICT",
        "MISMATCH",
        {{"a.jpg", photobridge::DiffKind::kChanged}},
        "final digest differs",
    };
    const std::string rendered = photobridge::RenderAuditReport(report);
    EXPECT_EQ(
        rendered,
        "{\"action\":\"TARGET_CONFLICT\",\"diff\":[{\"kind\":\"changed\",\"path\":\"a.jpg\"}],\"error\":\"final digest differs\",\"plan_id\":\"plan-1\",\"task_id\":\"task-1\",\"verification\":\"MISMATCH\"}\n");
}
