#pragma once

#include <string>
#include <string_view>

namespace redclaw::service {

struct UpnpPortMappingResult {
    bool attempted = false;
    bool mapped = false;
    int internal_port = 0;
    int external_port = 0;
    std::string internal_ip;
    std::string external_ip;
    std::string status = "disabled";
    std::string detail;
};

[[nodiscard]] UpnpPortMappingResult attempt_upnp_udp_port_mapping(
    int local_port,
    std::string_view description = "RedClawDesktop ICE",
    std::string_view multicast_interface = {});

}  // namespace redclaw::service
