#include "photobridge/source/local_folder_source.h"

#include <utility>

#include "photobridge/filesystem/linux_directory_walker.h"
#include "photobridge/filesystem/linux_file_ops.h"
#include "photobridge/model/asset_classifier.h"

namespace photobridge {
namespace {

bool IsReceiverStagingPath(std::string_view path)
{
    const std::string_view temporary_suffix = ".pbtmp";
    if (path.size() >= temporary_suffix.size()
        && path.substr(path.size() - temporary_suffix.size())
            == temporary_suffix) {
        return true;
    }
    constexpr std::string_view marker = "/.tmp/";
    return path.find(marker) != std::string_view::npos
        || path.starts_with(".tmp/");
}

class PhysicalAssetForwardingSink final : public DirectoryEntrySink {
public:
    explicit PhysicalAssetForwardingSink(PhysicalAssetSink& sink)
        : sink_(sink) {}

    Status Add(DirectoryEntry entry) override
    {
        if (IsReceiverStagingPath(entry.relative_path.bytes())) {
            return Status::Ok();
        }
        auto asset = ClassifyPhysicalAsset(entry);
        if (!asset.has_value()) {
            return Status::Ok();
        }

        return sink_.Add(std::move(asset.value()));
    }

private:
    PhysicalAssetSink& sink_;
};

}  // namespace

LocalFolderSource::LocalFolderSource(std::filesystem::path root)
    : root_(std::move(root)) {}

Status LocalFolderSource::Scan(PhysicalAssetSink& sink)
{
    if (root_.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "local folder source root must not be empty");
    }

    LinuxFileOps file_ops;
    auto root_fd = file_ops.OpenRoot(
        root_,
        OpenRootMode::kExisting);
    if (!root_fd.ok()) {
        return root_fd.status();
    }

    PhysicalAssetForwardingSink forwarding_sink(sink);
    LinuxDirectoryWalker walker;
    return walker.Walk(root_fd.value().get(), forwarding_sink);
}

std::string_view LocalFolderSource::TypeName() const noexcept
{
    return "local-folder";
}

}  // namespace photobridge
