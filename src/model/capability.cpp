#include "photobridge/model/capability.h"

#include <algorithm>
#include <utility>

namespace photobridge {
namespace {

constexpr std::size_t CapabilityKindCount = 8;

std::size_t CapabilityKindIndex(CapabilityKind kind)
{
    return static_cast<std::size_t>(kind);
}

}  // namespace

bool IsKnownSupportLevel(SupportLevel level) noexcept
{
    return static_cast<std::size_t>(level) <= static_cast<std::size_t>(
        SupportLevel::kUnverifiable);
}

const char* SupportLevelName(SupportLevel level) noexcept
{
    switch (level) {
    case SupportLevel::kFull: return "full";
    case SupportLevel::kPartial: return "partial";
    case SupportLevel::kTransformed: return "transformed";
    case SupportLevel::kManifestOnly: return "manifest-only";
    case SupportLevel::kUnsupported: return "unsupported";
    case SupportLevel::kUnverifiable: return "unverifiable";
    }
    return "unknown";
}

const Capability* TargetCapabilities::Find(CapabilityKind kind) const noexcept
{
    for (const Capability& entry : entries) {
        if (entry.kind == kind) {
            return &entry;
        }
    }
    return nullptr;
}

Status TargetCapabilities::Require(
    CapabilityKind kind,
    SupportLevel level) const
{
    const Capability* capability = Find(kind);
    if (capability == nullptr || capability->level != level) {
        return Status(
            StatusCode::kInvalidArgument,
            "target capability does not provide required "
                + std::to_string(static_cast<int>(kind))
                + "=" + SupportLevelName(level));
    }
    return Status::Ok();
}

Status TargetCapabilities::Validate() const
{
    if (version.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "target capability version must not be empty");
    }
    if (entries.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "target capabilities must contain at least one entry");
    }

    std::vector<bool> seen(CapabilityKindCount, false);
    std::size_t previous_kind = 0;
    bool first = true;
    for (const Capability& entry : entries) {
        const std::size_t kind = CapabilityKindIndex(entry.kind);
        if (kind >= CapabilityKindCount) {
            return Status(
                StatusCode::kInvalidArgument,
                "target capability contains an unknown kind");
        }
        if (!IsKnownSupportLevel(entry.level)) {
            return Status(
                StatusCode::kInvalidArgument,
                "target capability contains an unknown support level");
        }
        if (seen[kind]) {
            return Status(
                StatusCode::kInvalidArgument,
                "target capability kinds must be unique");
        }
        if (!first && kind <= previous_kind) {
            return Status(
                StatusCode::kInvalidArgument,
                "target capability entries must be in canonical order");
        }
        if (entry.representation.empty() || entry.reason.empty()) {
            return Status(
                StatusCode::kInvalidArgument,
                "target capability representation and reason must not be empty");
        }
        seen[kind] = true;
        previous_kind = kind;
        first = false;
    }
    return Status::Ok();
}

TargetCapabilities LocalDirectoryCapabilitiesV1()
{
    return TargetCapabilities{
        "directory-v1",
        {
            {
                CapabilityKind::kRegularFiles,
                SupportLevel::kFull,
                "regular-file",
                "L0 publishes assets as regular files",
            },
            {
                CapabilityKind::kNestedDirectories,
                SupportLevel::kFull,
                "hierarchical-directory",
                "relative target paths may contain nested directories",
            },
            {
                CapabilityKind::kByteExactContents,
                SupportLevel::kFull,
                "byte-exact",
                "the target stores copied bytes without media transformation",
            },
            {
                CapabilityKind::kAtomicNoReplacePublish,
                SupportLevel::kFull,
                "same-directory-rename-no-replace",
                "publication uses a temporary file and atomic no-replace rename",
            },
            {
                CapabilityKind::kDurableFileAndDirectorySync,
                SupportLevel::kFull,
                "fdatasync-and-directory-fsync",
                "the commit protocol can request file and directory durability",
            },
            {
                CapabilityKind::kSymlinks,
                SupportLevel::kUnsupported,
                "regular-files-only",
                "L0 does not create or adopt symlink target semantics",
            },
            {
                CapabilityKind::kCaseFoldSemantics,
                SupportLevel::kUnverifiable,
                "filesystem-dependent",
                "case folding must be probed or rejected by the path planner",
            },
            {
                CapabilityKind::kUnicodeNormalization,
                SupportLevel::kUnverifiable,
                "filesystem-dependent",
                "Unicode normalization must not be assumed by the target contract",
            },
        },
    };
}

}  // namespace photobridge
