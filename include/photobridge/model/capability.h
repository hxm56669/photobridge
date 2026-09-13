#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "photobridge/common/status.h"

namespace photobridge {

enum class SupportLevel {
    kFull,
    kPartial,
    kTransformed,
    kManifestOnly,
    kUnsupported,
    kUnverifiable,
};

bool IsKnownSupportLevel(SupportLevel level) noexcept;
const char* SupportLevelName(SupportLevel level) noexcept;

// CapabilityKind is deliberately about target semantics, not implementation
// details. Path collision policy remains a planner concern for C3.
enum class CapabilityKind {
    kRegularFiles,
    kNestedDirectories,
    kByteExactContents,
    kAtomicNoReplacePublish,
    kDurableFileAndDirectorySync,
    kSymlinks,
    kCaseFoldSemantics,
    kUnicodeNormalization,
};

struct Capability {
    CapabilityKind kind;
    SupportLevel level;
    std::string representation;
    std::string reason;
};

struct TargetCapabilities {
    std::string version;
    std::vector<Capability> entries;

    const Capability* Find(CapabilityKind kind) const noexcept;
    Status Require(CapabilityKind kind, SupportLevel level) const;
    Status Validate() const;
};

// Returns the stable capability contract for the L0 Linux local-directory
// target. This function is pure and does not probe or modify the filesystem.
TargetCapabilities LocalDirectoryCapabilitiesV1();

}  // namespace photobridge
