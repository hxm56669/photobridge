#include "photobridge/pipeline/pipeline_support.h"

namespace photobridge::pipeline {

namespace {

struct VerificationSummary {
    std::uint64_t bytes = 0;
    DiffFileState expected;
    DiffFileState observed;
};

StatusOr<VerificationSummary> VerifyPlanAsset(
    FileOps& file_ops,
    int source_root_fd,
    int target_root_fd,
    const MinimalPlanAsset& plan_asset,
    std::span<std::byte> buffer)
{
    if (plan_asset.target_path.components().size() != 1) {
        return Status(
            StatusCode::kInvalidArgument,
            "single-thread verification requires a single target path component");
    }

    const PhysicalAsset source_asset{
        plan_asset.source_path,
        plan_asset.source_identity,
        AssetKind::kUnknown,
        {},
    };
    auto source_guard = MutationGuard::Open(
        file_ops,
        source_root_fd,
        source_asset);
    if (!source_guard.ok()) return source_guard.status();
    Status status = source_guard.value().VerifyBeforeRead();
    if (!status.ok()) return status;

    Blake3Hasher source_hasher;
    auto source_verification = VerifyBinary(
        file_ops,
        source_hasher,
        source_guard.value().fd(),
        plan_asset.source_identity.size,
        std::nullopt,
        buffer);
    if (!source_verification.ok()) return source_verification.status();
    status = source_guard.value().VerifyAfterRead();
    if (!status.ok()) return status;
    if (source_verification.value().target_before
            != source_guard.value().manifest_identity()
        || source_verification.value().target_after
            != source_guard.value().manifest_identity()
        || source_verification.value().bytes_read
            != plan_asset.source_identity.size) {
        return Status(
            StatusCode::kInternal,
            "source changed during independent verification");
    }

    auto target_fd = file_ops.OpenSource(target_root_fd, plan_asset.target_path);
    if (!target_fd.ok()) return target_fd.status();
    Blake3Hasher target_hasher;
    auto target_verification = VerifyBinary(
        file_ops,
        target_hasher,
        target_fd.value().get(),
        plan_asset.source_identity.size,
        std::optional<Digest>(source_verification.value().target_digest),
        buffer);
    if (!target_verification.ok()) return target_verification.status();
    return VerificationSummary{
        target_verification.value().bytes_read,
        DiffFileState{
            std::string(plan_asset.target_path.bytes()),
            source_verification.value().bytes_read,
            source_verification.value().target_digest,
        },
        DiffFileState{
            std::string(plan_asset.target_path.bytes()),
            target_verification.value().bytes_read,
            target_verification.value().target_digest,
        },
    };
}

}  // namespace

class VerifyService final {
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
        if (artifact.value().plan.assets().empty()) {
            return Status(
                StatusCode::kInvalidArgument,
                "verification plan must contain at least one asset");
        }

        auto connection = SqliteConnection::Open(layout.value().database);
        if (!connection.ok()) {
            return connection.status();
        }
        const Status schema_status = EnsureSchema(connection.value());
        if (!schema_status.ok()) {
            return schema_status;
        }

        TaskRuntimeRepository repository(connection.value());
        auto source_root = ReadManifestSourceRoot(
            connection.value(),
            artifact.value().plan.source_manifest_id());
        if (!source_root.ok()) {
            return source_root.status();
        }

        LinuxFileOps file_ops;
        auto source_root_fd = file_ops.OpenRoot(
            std::filesystem::path(source_root.value()),
            OpenRootMode::kExisting);
        if (!source_root_fd.ok()) {
            return source_root_fd.status();
        }

        auto target_root_fd = file_ops.OpenRoot(
            std::filesystem::path(artifact.value().plan.target_root()),
            OpenRootMode::kExisting);
        if (!target_root_fd.ok()) {
            return target_root_fd.status();
        }

        std::array<std::byte, 1024 * 1024> buffer{};
        std::uint64_t verified_bytes = 0;
        std::vector<DiffFileState> expected_states;
        std::vector<DiffFileState> observed_states;
        expected_states.reserve(artifact.value().plan.assets().size());
        observed_states.reserve(artifact.value().plan.assets().size());
        for (const MinimalPlanAsset& plan_asset : artifact.value().plan.assets()) {
            auto task = TaskSpecForPlanAsset(plan_asset);
            if (!task.ok()) return task.status();
            auto runtime = repository.ReadRuntime(
                artifact.value().plan_id,
                task.value().id);
            if (!runtime.ok()) return runtime.status();
            if (runtime.value().state != TaskState::kSucceeded) {
                return Status(
                    StatusCode::kInvalidArgument,
                    "binary verification requires every task to be succeeded");
            }
            auto bytes = VerifyPlanAsset(
                file_ops,
                source_root_fd.value().get(),
                target_root_fd.value().get(),
                plan_asset,
                std::span<std::byte>(buffer));
            if (!bytes.ok()) return bytes.status();
            if (verified_bytes > std::numeric_limits<std::uint64_t>::max()
                    - bytes.value().bytes) {
                return Status(
                    StatusCode::kInternal,
                    "verified byte count overflowed");
            }
            verified_bytes += bytes.value().bytes;
            expected_states.push_back(std::move(bytes.value().expected));
            observed_states.push_back(std::move(bytes.value().observed));
        }
        const std::vector<DiffEntry> diff = DiffFileStates(
            expected_states,
            observed_states);
        context.out << RenderAuditReport(AuditReport{
            artifact.value().plan_id,
            {},
            "verify",
            diff.empty() ? "IDENTICAL" : "MISMATCH",
            diff,
            diff.empty() ? "" : "target differs from source",
        });
        if (!diff.empty()) {
            return Status(
                StatusCode::kInternal,
                "target binary verification did not match source");
        }
        context.out << "verify completed: " << artifact.value().plan_id
                    << " tasks=" << artifact.value().plan.assets().size()
                    << " status=IDENTICAL bytes=" << verified_bytes
                    << "\n";
        return Status::Ok();
    
    }
};


Status RunVerifyService(
    const StatusOr<WorkspaceLayout>& layout,
    const std::string& input_path,
    CommandContext& context)
{
    return VerifyService::Execute(layout, input_path, context);
}

}  // namespace photobridge::pipeline
