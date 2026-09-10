#pragma once

#include <compare>
#include <cstdint>
#include <optional>

namespace photobridge {

struct FileIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    std::int64_t ctime_ns = 0;
    std::optional<std::uint64_t> mount_id;

    auto operator<=>(const FileIdentity&) const = default;
};

}  // namespace photobridge
