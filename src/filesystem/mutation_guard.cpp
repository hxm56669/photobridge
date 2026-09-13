#include "photobridge/filesystem/mutation_guard.h"

#include <string>
#include <utility>

namespace photobridge {
namespace {

Status IdentityMismatch(const char* phase)
{
    return Status(
        StatusCode::kInternal,
        std::string("source file identity changed ") + phase);
}

Status VerifyIdentity(
    FileOps& file_ops,
    int fd,
    const FileIdentity& expected,
    const char* phase)
{
    auto actual = file_ops.StatFd(fd);
    if (!actual.ok()) {
        return actual.status();
    }
    if (actual.value() != expected) {
        return IdentityMismatch(phase);
    }
    return Status::Ok();
}

}  // namespace

MutationGuard::MutationGuard(
    FileOps& file_ops,
    UniqueFd fd,
    FileIdentity manifest_identity,
    FileIdentity opened_identity)
    : file_ops_(file_ops),
      fd_(std::move(fd)),
      manifest_identity_(std::move(manifest_identity)),
      opened_identity_(std::move(opened_identity)) {}

StatusOr<MutationGuard> MutationGuard::Open(
    FileOps& file_ops,
    int source_root_fd,
    const PhysicalAsset& asset)
{
    auto fd = file_ops.OpenSource(source_root_fd, asset.relative_path);
    if (!fd.ok()) {
        return fd.status();
    }

    auto opened_identity = file_ops.StatFd(fd.value().get());
    if (!opened_identity.ok()) {
        return opened_identity.status();
    }
    if (opened_identity.value() != asset.identity) {
        return IdentityMismatch("before read");
    }

    return MutationGuard(
        file_ops,
        std::move(fd.value()),
        asset.identity,
        opened_identity.value());
}

Status MutationGuard::VerifyBeforeRead()
{
    auto current = file_ops_.StatFd(fd_.get());
    if (!current.ok()) {
        return current.status();
    }
    if (current.value() != manifest_identity_) {
        return IdentityMismatch("before read");
    }
    opened_identity_ = current.value();
    return Status::Ok();
}

Status MutationGuard::VerifyAfterRead()
{
    return VerifyIdentity(
        file_ops_,
        fd_.get(),
        opened_identity_,
        "during read");
}

int MutationGuard::fd() const noexcept
{
    return fd_.get();
}

const FileIdentity& MutationGuard::manifest_identity() const noexcept
{
    return manifest_identity_;
}

}  // namespace photobridge
