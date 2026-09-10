#include "photobridge/app/workspace_service.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

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

        return Status::Ok();
    }

    std::ofstream database(
        layout.database,
        std::ios::binary | std::ios::out);
    if (!database) {
        return Status(
            StatusCode::kIoError,
            "cannot create workspace database: "
                + layout.database.string());
    }

    return Status::Ok();
}

}  // namespace photobridge
