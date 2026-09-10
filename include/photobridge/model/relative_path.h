#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "photobridge/common/status_or.h"

namespace photobridge {

class RelativePath {
public:
    static StatusOr<RelativePath> Parse(std::string raw_bytes);

    std::string_view bytes() const noexcept;
    std::string DisplayString() const;
    const std::vector<std::string>& components() const noexcept;

private:
    RelativePath(
        std::string raw_bytes,
        std::vector<std::string> components);

    std::string raw_bytes_;
    std::vector<std::string> components_;
};

}  // namespace photobridge
