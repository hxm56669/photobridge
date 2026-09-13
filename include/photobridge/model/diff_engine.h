#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "photobridge/common/digest.h"

namespace photobridge {

struct DiffFileState {
    std::string path;
    std::uint64_t size = 0;
    std::optional<Digest> digest;
};

enum class DiffKind {
    kAdded,
    kRemoved,
    kChanged,
};

struct DiffEntry {
    std::string path;
    DiffKind kind = DiffKind::kChanged;
};

std::vector<DiffEntry> DiffFileStates(
    std::span<const DiffFileState> expected,
    std::span<const DiffFileState> observed);

}  // namespace photobridge
