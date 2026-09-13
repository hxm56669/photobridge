#pragma once

#include "photobridge/common/status.h"
#include "photobridge/common/status_or.h"
#include "photobridge/common/unique_fd.h"
#include "photobridge/filesystem/file_ops.h"
#include "photobridge/model/file_identity.h"
#include "photobridge/model/physical_asset.h"

namespace photobridge {

class MutationGuard final {
public:
    static StatusOr<MutationGuard> Open(
        FileOps& file_ops,
        int source_root_fd,
        const PhysicalAsset& asset);

    MutationGuard(const MutationGuard&) = delete;
    MutationGuard& operator=(const MutationGuard&) = delete;

    MutationGuard(MutationGuard&&) noexcept = default;
    MutationGuard& operator=(MutationGuard&&) noexcept = delete;

    Status VerifyBeforeRead();
    Status VerifyAfterRead();

    int fd() const noexcept;
    const FileIdentity& manifest_identity() const noexcept;

private:
    MutationGuard(
        FileOps& file_ops,
        UniqueFd fd,
        FileIdentity manifest_identity,
        FileIdentity opened_identity);

    FileOps& file_ops_;
    UniqueFd fd_;
    FileIdentity manifest_identity_;
    FileIdentity opened_identity_;
};

}  // namespace photobridge
