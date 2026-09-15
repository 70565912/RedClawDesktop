#pragma once
#include <array>
#include <string>
#include <openssl/rand.h>
namespace redclaw::workspace {
inline std::string new_terminal_identity() {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(32);
    for (auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
}
