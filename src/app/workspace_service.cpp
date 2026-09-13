#include "photobridge/app/workspace_service.h"

#include <filesystem>
#include <string>
#include <system_error>

#include "photobridge/app/sqlite_connection.h"
#include "photobridge/app/sqlite_schema.h"
#include "photobridge/app/workspace_layout.h"

namespace photobridge {
namespace {

Status FileSystemError(
    const std::filesystem::path& path,
    const std::error_code& error)
{
    return Status(
        StatusCode::kIoError,
        "workspace operation failed for '" + path.string()
            + "': " + error.message());
}

}  // namespace

Status WorkspaceService::Initialize(
    const WorkspaceLayout& layout) const
{
    const std::filesystem::path directories[] = {
        layout.root,
        layout.manifests,
        layout.plans,
        layout.reports,
        layout.logs,
        layout.locks,
    };

    for (const auto& directory : directories) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            return FileSystemError(directory, error);
        }
    }

    std::error_code error;
    const bool database_exists =
        std::filesystem::exists(layout.database, error);
    if (error) {
        return FileSystemError(layout.database, error);
    }

    if (database_exists) {
        if (std::filesystem::is_directory(layout.database, error)) {
            if (error) {
                return FileSystemError(layout.database, error);
            }

            return Status(
                StatusCode::kAlreadyExists,
                "workspace database path is a directory: "
                    + layout.database.string());
        }

    }

    auto connection = SqliteConnection::Open(layout.database);
    if (!connection.ok()) {
        return connection.status();
    }

    const Status schema_status = EnsureSchema(connection.value());
    if (!schema_status.ok()) {
        return schema_status;
    }

    return Status::Ok();
}

}  // namespace photobridge
