#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace redclaw::net {

// A successful router mapping, not the socket's bind address/port.
struct IceUdpPortMapping {
    std::string internal_address;
    std::uint16_t internal_port = 0;
    std::string external_address;
    std::uint16_t external_port = 0;
};

// Adds a standard srflx route without replacing native host/STUN/TURN routes.
// Only the IPv4 UDP component-1 host candidate matching the mapped socket qualifies.
[[nodiscard]] std::optional<std::string> mapped_udp_candidate(
    std::string_view host_candidate, const IceUdpPortMapping& mapping);
[[nodiscard]] std::string with_mapped_udp_candidates(
    std::string_view sdp, const IceUdpPortMapping& mapping);

} // namespace redclaw::net
