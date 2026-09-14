#include "photobridge/pipeline/pipeline_support.h"

namespace photobridge::pipeline {

namespace {

std::string TaskStateName(TaskState state)
{
    switch (state) {
    case TaskState::kPlanned:
        return "PLANNED";
    case TaskState::kReady:
        return "READY";
    case TaskState::kRunning:
        return "RUNNING";
    case TaskState::kSucceeded:
        return "SUCCEEDED";
    case TaskState::kRetryable:
        return "RETRYABLE";
    case TaskState::kFailed:
        return "FAILED";
    case TaskState::kNeedsReview:
        return "NEEDS_REVIEW";
    case TaskState::kInconsistent:
        return "INCONSISTENT";
    case TaskState::kSkipped:
        return "SKIPPED";
    }
    return "UNKNOWN";
}

}  // namespace

class StatusService final {
public:
    static Status Execute(
        const StatusOr<WorkspaceLayout>& layout,
        const std::string& input_path,
        CommandContext& context)
    {
        auto plan_bytes = ReadPlanFile(std::filesystem::path(input_path));
        if (!plan_bytes.ok()) {
            return plan_bytes.status();
        }
        auto artifact = ReadFrozenPlan(plan_bytes.value());
        if (!artifact.ok()) {
            return artifact.status();
        }

        auto connection = SqliteConnection::Open(layout.value().database);
        if (!connection.ok()) {
            return connection.status();
        }

        PipelineStatement plan_statement(
            connection.value().native_handle(),
            "SELECT artifact_digest, semantic_digest FROM migration_plan "
            "WHERE plan_id = ?1;");
        if (plan_statement.result() != SQLITE_OK) {
            return SqliteReadError(
                connection.value().native_handle(),
                "prepare status plan lookup");
        }
        Status status = BindPipelineText(
            plan_statement.get(),
            1,
            artifact.value().plan_id);
        if (!status.ok()) return status;
        const int plan_step = sqlite3_step(plan_statement.get());
        if (plan_step == SQLITE_DONE) {
            return Status(
                StatusCode::kNotFound,
                "materialized plan was not found: "
                    + artifact.value().plan_id);
        }
        if (plan_step != SQLITE_ROW) {
            return SqliteReadError(
                connection.value().native_handle(),
                "read status plan lookup");
        }
        for (const int column : {0, 1}) {
            if (sqlite3_column_type(plan_statement.get(), column) != SQLITE_BLOB
                || sqlite3_column_bytes(plan_statement.get(), column)
                    != static_cast<int>(artifact.value().artifact_digest.bytes.size())) {
                return Status(
                    StatusCode::kInternal,
                    "status plan digest has an invalid type or size");
            }
        }
        const auto digest_matches = [
            &plan_statement](int column, const Digest& expected) {
            return std::equal(
                expected.bytes.begin(),
                expected.bytes.end(),
                static_cast<const std::byte*>(sqlite3_column_blob(
                    plan_statement.get(),
                    column)));
        };
        if (!digest_matches(0, artifact.value().artifact_digest)
            || !digest_matches(1, artifact.value().semantic_digest)) {
            return Status(
                StatusCode::kInternal,
                "frozen plan digest does not match SQLite binding");
        }

        auto counts = ReadTaskStateCounts(
            connection.value(),
            artifact.value().plan_id);
        if (!counts.ok()) {
            return counts.status();
        }
        std::uint64_t task_count = 0;
        for (const std::uint64_t count : counts.value()) {
            task_count += count;
        }
        context.out << "status: " << artifact.value().plan_id
                    << " tasks=" << task_count;
        for (std::size_t index = 0; index < counts.value().size(); ++index) {
            if (counts.value()[index] == 0) continue;
            context.out << " "
                        << TaskStateName(static_cast<TaskState>(index))
                        << "=" << counts.value()[index];
        }
        context.out << "\n";
        return Status::Ok();
    
    }
};


Status RunStatusService(
    const StatusOr<WorkspaceLayout>& layout,
    const std::string& input_path,
    CommandContext& context)
{
    return StatusService::Execute(layout, input_path, context);
}

}  // namespace photobridge::pipeline
