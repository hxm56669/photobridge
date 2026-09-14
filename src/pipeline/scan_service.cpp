#include "photobridge/pipeline/pipeline_support.h"

namespace photobridge::pipeline {

class ScanService final {
public:
    static Status Execute(
        const StatusOr<WorkspaceLayout>& layout,
        const std::string& input_path,
        CommandContext& context)
    {
        auto connection = SqliteConnection::Open(layout.value().database);
        if (!connection.ok()) {
            return connection.status();
        }

        const Status schema_status = EnsureSchema(connection.value());
        if (!schema_status.ok()) {
            return schema_status;
        }

        LocalFolderSource source{std::filesystem::path(input_path)};
        SqliteManifestBuilder builder(connection.value());
        const Status begin_status = builder.Begin({
            input_path,
            std::string(source.TypeName()),
            input_path,
        });
        if (!begin_status.ok()) {
            return begin_status;
        }

        const Status scan_status = source.Scan(builder);
        if (!scan_status.ok()) {
            return scan_status;
        }

        auto frozen = builder.Freeze();
        if (!frozen.ok()) {
            return frozen.status();
        }

        context.out << "scan completed: "
                    << frozen.value().manifest_id
                    << " assets=" << frozen.value().asset_count
                    << " digest=" << frozen.value().manifest_digest.ToHex()
                    << "\n";
        return Status::Ok();
    
    }
};


Status RunScanService(
    const StatusOr<WorkspaceLayout>& layout,
    const std::string& input_path,
    CommandContext& context)
{
    return ScanService::Execute(layout, input_path, context);
}

}  // namespace photobridge::pipeline
