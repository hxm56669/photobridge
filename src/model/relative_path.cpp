#include "photobridge/model/relative_path.h"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <utility>

namespace photobridge {

RelativePath::RelativePath(
    std::string raw_bytes,
    std::vector<std::string> components)
    : raw_bytes_(std::move(raw_bytes)),
      components_(std::move(components))
{
}

StatusOr<RelativePath> RelativePath::Parse(std::string raw_bytes)
{
    if (raw_bytes.empty()) {
        return Status(
            StatusCode::kInvalidArgument,
            "relative path must not be empty");
    }

    if (raw_bytes.find('\0') != std::string::npos) {
        return Status(
            StatusCode::kInvalidArgument,
            "relative path must not contain NUL");
    }

    if (raw_bytes.front() == '/') {
        return Status(
            StatusCode::kInvalidArgument,
            "relative path must not be absolute");
    }

    std::vector<std::string> components;
    std::size_t start = 0;
    while (start <= raw_bytes.size()) {
        const std::size_t separator = raw_bytes.find('/', start);
        const std::size_t length =
            separator == std::string::npos
                ? raw_bytes.size() - start
                : separator - start;
        const std::string component = raw_bytes.substr(start, length);

        if (component.empty()) {
            return Status(
                StatusCode::kInvalidArgument,
                "relative path must not contain empty components");
        }

        if (component == "." || component == "..") {
            return Status(
                StatusCode::kInvalidArgument,
                "relative path must not contain dot components");
        }

        components.push_back(component);

        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }

    return RelativePath(
        std::move(raw_bytes),
        std::move(components));
}

std::string_view RelativePath::bytes() const noexcept
{
    return raw_bytes_;
}

std::string RelativePath::DisplayString() const
{
    std::ostringstream display;
    display << std::uppercase << std::hex;

    for (const unsigned char byte : raw_bytes_) {
        if (byte >= 0x20 && byte <= 0x7E && byte != '\\') {
            display << static_cast<char>(byte);
        } else if (byte == '\\') {
            display << "\\\\";
        } else {
            display << "\\x"
                    << std::setw(2)
                    << std::setfill('0')
                    << static_cast<unsigned int>(byte);
        }
    }

    return display.str();
}

const std::vector<std::string>& RelativePath::components() const noexcept
{
    return components_;
}

}  // namespace photobridge
