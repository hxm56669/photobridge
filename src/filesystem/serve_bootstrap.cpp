#include "photobridge/lan/serve_bootstrap.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <random>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>

#include <qrcodegen.hpp>

#include <arpa/inet.h>

namespace photobridge {
namespace {

constexpr std::string_view kBase64UrlAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string EncodeBase64Url(const std::array<unsigned char, 32>& bytes)
{
    std::string encoded;
    encoded.reserve(43);

    std::uint32_t buffer = 0;
    int bits = 0;
    for (const unsigned char byte : bytes) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            encoded.push_back(kBase64UrlAlphabet[(buffer >> bits) & 0x3f]);
        }
    }
    if (bits > 0) {
        encoded.push_back(kBase64UrlAlphabet[(buffer << (6 - bits)) & 0x3f]);
    }
    return encoded;
}

StatusOr<std::string> GenerateToken()
{
    try {
        std::random_device random;
        std::array<unsigned char, 32> bytes{};
        for (auto& byte : bytes) {
            byte = static_cast<unsigned char>(random() & 0xffU);
        }
        return EncodeBase64Url(bytes);
    } catch (const std::exception& error) {
        return Status(
            StatusCode::kInternal,
            std::string("unable to generate serve token: ") + error.what());
    }
}

std::string SelectLanAddress()
{
    struct ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        return "127.0.0.1";
    }

    std::vector<std::string> candidates;
    for (const auto* current = interfaces; current != nullptr;
         current = current->ifa_next) {
        if (current->ifa_addr == nullptr
            || (current->ifa_flags & IFF_UP) == 0
            || (current->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        char address[INET6_ADDRSTRLEN]{};
        if (current->ifa_addr->sa_family == AF_INET) {
            const auto* value = reinterpret_cast<const sockaddr_in*>(
                current->ifa_addr);
            if (inet_ntop(AF_INET, &value->sin_addr, address, sizeof(address))) {
                candidates.emplace_back(address);
            }
        } else if (current->ifa_addr->sa_family == AF_INET6) {
            const auto* value = reinterpret_cast<const sockaddr_in6*>(
                current->ifa_addr);
            if (inet_ntop(AF_INET6, &value->sin6_addr, address, sizeof(address))) {
                candidates.emplace_back(address);
            }
        }
    }
    freeifaddrs(interfaces);

    if (candidates.empty()) {
        return "127.0.0.1";
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates.front();
}

std::string UrlHost(std::string_view address)
{
    if (address.find(':') != std::string_view::npos) {
        return "[" + std::string(address) + "]";
    }
    return std::string(address);
}

std::string RenderQrCode(std::string_view value)
{
    const auto qr = qrcodegen::QrCode::encodeText(
        std::string(value).c_str(),
        qrcodegen::QrCode::Ecc::MEDIUM);
    constexpr int border = 2;
    std::string result;
    for (int y = -border; y < qr.getSize() + border; ++y) {
        for (int x = -border; x < qr.getSize() + border; ++x) {
            result += qr.getModule(x, y) ? "██" : "  ";
        }
        result.push_back('\n');
    }
    return result;
}

}  // namespace

StatusOr<ServeBootstrap> ServeBootstrap::Create(ServeOptions options)
{
    if (options.port == 0) {
        return Status(
            StatusCode::kInvalidArgument,
            "serve port must be between 1 and 65535");
    }

    if (options.bind_address.empty()) {
        options.bind_address = SelectLanAddress();
    }

    auto token = GenerateToken();
    if (!token.ok()) {
        return token.status();
    }

    ServeBootstrap bootstrap;
    bootstrap.bind_address = std::move(options.bind_address);
    bootstrap.port = options.port;
    bootstrap.token = std::move(token.value());
    bootstrap.url = "http://" + UrlHost(bootstrap.bind_address) + ":"
        + std::to_string(bootstrap.port) + "/?t=" + bootstrap.token;
    bootstrap.qr_code = RenderQrCode(bootstrap.url);
    return bootstrap;
}

}  // namespace photobridge
