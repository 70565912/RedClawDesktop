#include "redclaw/net/ice_udp_mapping.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <sstream>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace redclaw::net {
namespace {
bool ipv4(std::string_view text) {
    if (text.empty() || text.size() > 15 || text.find('\0') != std::string_view::npos) return false;
    in_addr address{};
    return inet_pton(AF_INET, std::string(text).c_str(), &address) == 1
        && address.s_addr != 0 && (ntohl(address.s_addr) >> 24U) < 224U;
}
bool number(std::string_view text, std::uint32_t& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
} // namespace

std::optional<std::string> mapped_udp_candidate(
    std::string_view host_candidate, const IceUdpPortMapping& mapping) {
    if (!mapping.internal_port || !mapping.external_port
        || !ipv4(mapping.internal_address) || !ipv4(mapping.external_address)
        || host_candidate.size() > 4096
        || host_candidate.find_first_of("\r\n\0", 0, 3) != std::string_view::npos) return {};
    std::array<std::string, 8> fields;
    std::istringstream input{std::string(host_candidate)};
    for (auto& field : fields) if (!(input >> field)) return {};
    if ((!fields[0].starts_with("candidate:") && !fields[0].starts_with("a=candidate:"))
        || fields[1] != "1" || (fields[2] != "udp" && fields[2] != "UDP")
        || fields[4] != mapping.internal_address || fields[6] != "typ" || fields[7] != "host") return {};
    std::uint32_t port = 0, priority = 0;
    if (!number(fields[5], port) || port != mapping.internal_port
        || !number(fields[3], priority)) return {};
    if (mapping.external_address == mapping.internal_address && mapping.external_port == port) return {};
    // RFC 8445 srflx type preference, retaining the base's local/component preference.
    priority = (100U << 24U) | (priority & 0x00ffffffU);
    return "candidate:upnp 1 UDP " + std::to_string(priority) + " "
        + mapping.external_address + " " + std::to_string(mapping.external_port)
        + " typ srflx raddr " + mapping.internal_address + " rport " + std::to_string(port);
}

std::string with_mapped_udp_candidates(std::string_view sdp, const IceUdpPortMapping& mapping) {
    if (sdp.size() > 65536) return std::string(sdp);
    std::vector<std::string> lines;
    std::istringstream input{std::string(sdp)};
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    std::string result;
    bool added = false, section_added = false;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].starts_with("m=")) section_added = false;
        result += lines[i] + "\r\n";
        if (section_added) continue;
        if (const auto candidate = mapped_udp_candidate(lines[i], mapping)) {
            const auto mapped = "a=" + *candidate;
            // Deduplicate within the media section, never across unrelated mids.
            auto first = lines.begin() + static_cast<std::ptrdiff_t>(i);
            while (first != lines.begin() && !first->starts_with("m=")) --first;
            auto last = lines.begin() + static_cast<std::ptrdiff_t>(i + 1);
            while (last != lines.end() && !last->starts_with("m=")) ++last;
            if (std::find(first, last, mapped) == last) {
                result += mapped + "\r\n";
                added = section_added = true;
            }
        }
    }
    return added ? result : std::string(sdp);
}
} // namespace redclaw::net
