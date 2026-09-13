#pragma once

#include <string>
#include <string_view>

namespace photobridge {

struct UploadPageResponse {
    std::string content_type;
    std::string body;
};

// Returns the complete self-contained page served for GET /.
UploadPageResponse GetUploadPageResponse();

bool ConstantTimeTokenEquals(
    std::string_view provided,
    std::string_view expected) noexcept;

}  // namespace photobridge
