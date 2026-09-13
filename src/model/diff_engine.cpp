#include "photobridge/model/diff_engine.h"

#include <algorithm>
#include <map>

namespace photobridge {

std::vector<DiffEntry> DiffFileStates(
    std::span<const DiffFileState> expected,
    std::span<const DiffFileState> observed)
{
    const auto by_path = [](std::span<const DiffFileState> states) {
        std::map<std::string, const DiffFileState*> result;
        for (const DiffFileState& state : states) {
            result.emplace(state.path, &state);
        }
        return result;
    };
    const auto expected_by_path = by_path(expected);
    const auto observed_by_path = by_path(observed);
    std::vector<DiffEntry> result;
    for (const auto& [path, state] : expected_by_path) {
        const auto observed_it = observed_by_path.find(path);
        if (observed_it == observed_by_path.end()) {
            result.push_back({path, DiffKind::kRemoved});
            continue;
        }
        const DiffFileState& actual = *observed_it->second;
        if (state->size != actual.size || state->digest != actual.digest) {
            result.push_back({path, DiffKind::kChanged});
        }
    }
    for (const auto& [path, state] : observed_by_path) {
        static_cast<void>(state);
        if (expected_by_path.find(path) == expected_by_path.end()) {
            result.push_back({path, DiffKind::kAdded});
        }
    }
    std::sort(
        result.begin(),
        result.end(),
        [](const DiffEntry& left, const DiffEntry& right) {
            if (left.path != right.path) return left.path < right.path;
            return left.kind < right.kind;
        });
    return result;
}

}  // namespace photobridge
