#include "photobridge/pipeline/pipeline_support.h"

#include <fstream>
#include <iterator>

namespace photobridge::pipeline {

namespace {

StatusOr<std::uint64_t> ReadUnsignedInteger(
    sqlite3_stmt* statement,
    int column,
    std::string_view field_name)
{
    if (sqlite3_column_type(statement, column) != SQLITE_INTEGER) {
        return Status(
            StatusCode::kInternal,
            std::string("manifest field is not an integer: ")
                + std::string(field_name));
    }

    const sqlite3_int64 value = sqlite3_column_int64(statement, column);
    if (value < 0) {
        return Status(
            StatusCode::kInternal,
            std::string("manifest field is negative: ")
                + std::string(field_name));
    }
    return static_cast<std::uint64_t>(value);
}

StatusOr<std::vector<PhysicalAsset>> ReadManifestAssets(
    SqliteConnection& connection,
    std::string_view manifest_id)
{
    sqlite3_stmt* statement = nullptr;
    const int prepare_result = sqlite3_prepare_v2(
        connection.native_handle(),
        "SELECT relative_path, device, inode, size, mtime_ns, ctime_ns, "
        "kind FROM physical_asset WHERE manifest_id = ? "
        "ORDER BY relative_path ASC;",
        -1,
        &statement,
        nullptr);
    if (prepare_result != SQLITE_OK) {
        return SqliteReadError(
            connection.native_handle(),
            "prepare manifest asset read");
    }

    const int bind_result = sqlite3_bind_text(
        statement,
        1,
        manifest_id.data(),
        static_cast<int>(manifest_id.size()),
        SQLITE_TRANSIENT);
    if (bind_result != SQLITE_OK) {
        sqlite3_finalize(statement);
        return SqliteReadError(
            connection.native_handle(),
            "bind manifest id for asset read");
    }

    std::vector<PhysicalAsset> assets;
    while (true) {
        const int step_result = sqlite3_step(statement);
        if (step_result == SQLITE_DONE) {
            break;
        }
        if (step_result != SQLITE_ROW) {
            const Status status = SqliteReadError(
                connection.native_handle(),
                "read manifest asset");
            sqlite3_finalize(statement);
            return status;
        }

        if (sqlite3_column_type(statement, 0) != SQLITE_BLOB) {
            sqlite3_finalize(statement);
            return Status(
                StatusCode::kInternal,
                "manifest relative path is not a blob");
        }
        const int path_size = sqlite3_column_bytes(statement, 0);
        const void* path_data = sqlite3_column_blob(statement, 0);
        if (path_size <= 0 || path_data == nullptr) {
            sqlite3_finalize(statement);
            return Status(
                StatusCode::kInternal,
                "manifest relative path is empty");
        }
        auto relative_path = RelativePath::Parse(
            std::string(
                static_cast<const char*>(path_data),
                static_cast<std::size_t>(path_size)));
        if (!relative_path.ok()) {
            sqlite3_finalize(statement);
            return relative_path.status();
        }

        auto device = ReadUnsignedInteger(statement, 1, "device");
        auto inode = ReadUnsignedInteger(statement, 2, "inode");
        auto size = ReadUnsignedInteger(statement, 3, "size");
        if (!device.ok() || !inode.ok() || !size.ok()) {
            const Status status = !device.ok()
                ? device.status()
                : (!inode.ok() ? inode.status() : size.status());
            sqlite3_finalize(statement);
            return status;
        }

        if (sqlite3_column_type(statement, 4) != SQLITE_INTEGER
            || sqlite3_column_type(statement, 5) != SQLITE_INTEGER
            || sqlite3_column_type(statement, 6) != SQLITE_INTEGER) {
            sqlite3_finalize(statement);
            return Status(
                StatusCode::kInternal,
                "manifest identity or kind field is not an integer");
        }

        const sqlite3_int64 kind = sqlite3_column_int64(statement, 6);
        if (kind < 0 || kind > static_cast<sqlite3_int64>(
                AssetKind::kUnknown)) {
            sqlite3_finalize(statement);
            return Status(
                StatusCode::kInternal,
                "manifest asset kind is unknown");
        }

        FileIdentity identity;
        identity.device = device.value();
        identity.inode = inode.value();
        identity.size = size.value();
        identity.mtime_ns = sqlite3_column_int64(statement, 4);
        identity.ctime_ns = sqlite3_column_int64(statement, 5);
        assets.push_back(PhysicalAsset{
            std::move(relative_path.value()),
            identity,
            static_cast<AssetKind>(kind),
            {},
        });
    }

    sqlite3_finalize(statement);
    return assets;
}

Status WritePlanFile(
    const std::filesystem::path& path,
    std::string_view bytes)
{
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error) {
        return Status(
            StatusCode::kIoError,
            "check plan artifact path: " + exists_error.message());
    }

    if (exists) {
        if (std::filesystem::is_directory(path, exists_error)) {
            return Status(
                StatusCode::kAlreadyExists,
                "plan artifact path is a directory: " + path.string());
        }
        if (exists_error) {
            return Status(
                StatusCode::kIoError,
                "inspect plan artifact path: " + exists_error.message());
        }

        std::ifstream existing(path, std::ios::binary);
        if (!existing) {
            return Status(
                StatusCode::kIoError,
                "open existing plan artifact: " + path.string());
        }
        const std::string existing_bytes{
            std::istreambuf_iterator<char>(existing),
            std::istreambuf_iterator<char>()};
        if (std::string_view(existing_bytes) == bytes) {
            return Status::Ok();
        }
        return Status(
            StatusCode::kAlreadyExists,
            "plan artifact already exists with different bytes: "
                + path.string());
    }

    LinuxFileOps file_ops;
    auto parent = file_ops.OpenRoot(path.parent_path(), OpenRootMode::kExisting);
    if (!parent.ok()) return parent.status();
    const std::string final_name = path.filename().string();
    const std::string temp_name = ".pbtmp." + final_name;
    auto temporary = file_ops.CreateTempNoReplace(
        parent.value().get(),
        temp_name,
        0600);
    if (!temporary.ok()) return temporary.status();

    const auto cleanup = [&file_ops, &parent, &temp_name]() {
        static_cast<void>(file_ops.UnlinkAt(parent.value().get(), temp_name));
    };
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto* data = reinterpret_cast<const std::byte*>(
            bytes.data() + offset);
        const std::size_t remaining = bytes.size() - offset;
        auto written = file_ops.Write(
            temporary.value().get(),
            std::span<const std::byte>(data, remaining));
        if (!written.ok()) {
            cleanup();
            return written.status();
        }
        if (written.value() == 0) {
            cleanup();
            return Status(
                StatusCode::kIoError,
                "plan artifact write made no progress");
        }
        offset += written.value();
    }
    Status status = file_ops.Fdatasync(temporary.value().get());
    if (!status.ok()) {
        cleanup();
        return status;
    }
    status = file_ops.RenameNoReplace(
        parent.value().get(),
        temp_name,
        parent.value().get(),
        final_name);
    if (!status.ok()) return status;
    return file_ops.FsyncDirectory(parent.value().get());
}

}  // namespace

class PlanService final {
public:
    static Status Execute(
        const StatusOr<WorkspaceLayout>& layout,
        const std::string& input_path,
        const std::string& target_path,
        CommandContext& context)
    {
        if (target_path.empty()) {
            return Status(
                StatusCode::kInvalidArgument,
                "plan stage requires a target root");
        }

        auto connection = SqliteConnection::Open(layout.value().database);
        if (!connection.ok()) {
            return connection.status();
        }
        const Status schema_status = EnsureSchema(connection.value());
        if (!schema_status.ok()) {
            return schema_status;
        }

        auto frozen_manifest = SqliteManifestBuilder::Reopen(
            connection.value(),
            input_path);
        if (!frozen_manifest.ok()) {
            return frozen_manifest.status();
        }
        auto manifest = frozen_manifest.value().frozen_manifest();
        if (!manifest.ok()) {
            return manifest.status();
        }

        auto physical_assets = ReadManifestAssets(
            connection.value(),
            manifest.value().manifest_id);
        if (!physical_assets.ok()) {
            return physical_assets.status();
        }

        std::vector<LogicalAsset> logical_assets;
        logical_assets.reserve(physical_assets.value().size());
        for (const PhysicalAsset& asset : physical_assets.value()) {
            auto logical = MapPhysicalAssetToLogicalAsset(asset);
            if (!logical.ok()) {
                return logical.status();
            }
            logical_assets.push_back(std::move(logical.value()));
        }

        const TargetCapabilities capabilities =
            LocalDirectoryCapabilitiesV1();
        auto loss_analysis = AnalyzeCapabilityLoss(capabilities);
        if (!loss_analysis.ok()) return loss_analysis.status();
        const MigrationPolicy policy{};
        TargetPathMapper mapper;
        auto mappings = mapper.MapAll(
            logical_assets,
            capabilities,
            policy);
        if (!mappings.ok()) {
            return mappings.status();
        }

        std::vector<MinimalPlanAsset> plan_assets;
        plan_assets.reserve(mappings.value().size());
        for (const PathMapping& mapping : mappings.value()) {
            const auto logical = std::find_if(
                logical_assets.begin(),
                logical_assets.end(),
                [&mapping](const LogicalAsset& candidate) {
                    return candidate.id == mapping.logical_asset_id;
                });
            if (logical == logical_assets.end()
                || !logical->source_path.has_value()
                || logical->members.size() != 1) {
                return Status(
                    StatusCode::kInternal,
                    "logical asset mapping cannot be bound to one source");
            }

            const auto physical = std::find_if(
                physical_assets.value().begin(),
                physical_assets.value().end(),
                [&logical](const PhysicalAsset& candidate) {
                    return PhysicalAssetIdFor(candidate)
                        == logical->members.front();
                });
            if (physical == physical_assets.value().end()) {
                return Status(
                    StatusCode::kInternal,
                    "logical asset member is missing from manifest");
            }

            plan_assets.push_back(MinimalPlanAsset{
                logical->id,
                logical->members.front(),
                logical->source_path.value(),
                mapping.target_path,
                physical->identity,
            });
        }

        auto plan = CanonicalMinimalPlan::Build(PlannerInput{
            manifest.value().manifest_id,
            manifest.value().manifest_digest,
            target_path,
            capabilities,
            policy,
            std::move(plan_assets),
        });
        if (!plan.ok()) {
            return plan.status();
        }

        const std::string plan_id = "plan-"
            + SemanticDigestFor(plan.value()).ToHex();
        auto artifact = WriteFrozenPlan(plan.value(), plan_id);
        if (!artifact.ok()) {
            return artifact.status();
        }

        const std::filesystem::path artifact_path = layout.value().plans
            / (plan_id + ".plan.jsonl");
        const Status write_status = WritePlanFile(
            artifact_path,
            artifact.value().bytes);
        if (!write_status.ok()) {
            return write_status;
        }

        context.out << "plan completed: " << artifact_path.string()
                    << " assets=" << artifact.value().plan.assets().size()
                    << " artifact_digest="
                    << artifact.value().artifact_digest.ToHex()
                    << " semantic_digest="
                    << artifact.value().semantic_digest.ToHex()
                    << " capability_losses="
                    << loss_analysis.value().losses.size()
                    << "\n";
        return Status::Ok();
    
    }
};


Status RunPlanService(
    const StatusOr<WorkspaceLayout>& layout,
    const std::string& input_path,
    const std::string& target_path,
    CommandContext& context)
{
    return PlanService::Execute(layout, input_path, target_path, context);
}

}  // namespace photobridge::pipeline
