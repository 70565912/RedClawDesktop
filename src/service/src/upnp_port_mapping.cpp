#include "redclaw/service/upnp_port_mapping.h"

#include <charconv>
#include <cstdint>
#include <string>

#if defined(REDCLAW_ENABLE_DHT_BACKENDS)
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

namespace redclaw::service {
namespace {

#if defined(REDCLAW_ENABLE_DHT_BACKENDS)
constexpr int kDefaultUpnpDiscoverDelayMilliseconds = 2000;

int parse_reserved_port(const char* reserved_port) {
    if (reserved_port == nullptr || reserved_port[0] == '\0') {
        return 0;
    }
    std::uint64_t parsed = 0;
    const char* begin = reserved_port;
    const char* end = reserved_port + std::char_traits<char>::length(reserved_port);
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || parsed == 0 || parsed > 65535) {
        return 0;
    }
    return static_cast<int>(parsed);
}
#endif

}  // namespace

UpnpPortMappingResult attempt_upnp_udp_port_mapping(
    int local_port,
    std::string_view description,
    std::string_view multicast_interface) {
    UpnpPortMappingResult result;
    result.attempted = true;
    result.internal_port = local_port;
    if (local_port <= 0 || local_port > 65535) {
        result.status = "invalid_port";
        result.detail = "ICE UDP port is outside 1..65535";
        return result;
    }

#if !defined(REDCLAW_ENABLE_DHT_BACKENDS)
    result.status = "unsupported";
    result.detail = "miniupnpc support is not built";
    return result;
#else
    int discover_error = 0;
    const std::string multicast_interface_string(multicast_interface);
    const char* multicast_interface_ptr = multicast_interface_string.empty()
        ? nullptr
        : multicast_interface_string.c_str();
    UPNPDev* devlist = upnpDiscover(
        kDefaultUpnpDiscoverDelayMilliseconds,
        multicast_interface_ptr,
        nullptr,
        UPNP_LOCAL_PORT_ANY,
        0,
        2,
        &discover_error);
    if (devlist == nullptr) {
        result.status = "no_igd";
        result.detail = discover_error == UPNPDISCOVER_SUCCESS
            ? "no UPnP IGD discovered"
            : (std::string("UPnP discovery failed with code ") + std::to_string(discover_error));
        return result;
    }

    UPNPUrls urls{};
    IGDdatas data{};
    char lan_address[64] = {};
#if defined(MINIUPNPC_API_VERSION) && MINIUPNPC_API_VERSION >= 18
    char wan_address[64] = {};
    const int igd_result = UPNP_GetValidIGD(
        devlist,
        &urls,
        &data,
        lan_address,
        static_cast<int>(sizeof(lan_address)),
        wan_address,
        static_cast<int>(sizeof(wan_address)));
#else
    const int igd_result = UPNP_GetValidIGD(
        devlist,
        &urls,
        &data,
        lan_address,
        static_cast<int>(sizeof(lan_address)));
#endif
    if (igd_result != 1 && igd_result != 2) {
        result.status = "no_igd";
        result.detail = igd_result == 0
            ? "no valid UPnP IGD was found"
            : "UPnP device was discovered but was not a connected IGD";
        freeUPNPDevlist(devlist);
        return result;
    }

    char external_ip[40] = {};
    if (UPNP_GetExternalIPAddress(
            urls.controlURL,
            data.first.servicetype,
            external_ip) == UPNPCOMMAND_SUCCESS) {
        result.external_ip = external_ip;
    }

    const std::string description_string(description);
    const char* description_ptr = description_string.empty() ? nullptr : description_string.c_str();
    const std::string port_text = std::to_string(local_port);
    char reserved_port[6] = {};
    int mapping_error = UPNP_AddAnyPortMapping(
        urls.controlURL,
        data.first.servicetype,
        port_text.c_str(),
        port_text.c_str(),
        lan_address,
        description_ptr,
        "UDP",
        nullptr,
        "0",
        reserved_port);
    if (mapping_error != UPNPCOMMAND_SUCCESS) {
        mapping_error = UPNP_AddPortMapping(
            urls.controlURL,
            data.first.servicetype,
            port_text.c_str(),
            port_text.c_str(),
            lan_address,
            description_ptr,
            "UDP",
            nullptr,
            "0");
    }

    if (mapping_error == UPNPCOMMAND_SUCCESS) {
        result.mapped = true;
        result.status = "mapped";
        result.external_port = parse_reserved_port(reserved_port);
        if (result.external_port == 0) {
            result.external_port = local_port;
        }
        result.detail = result.external_ip.empty()
            ? "miniupnpc mapped ICE UDP port"
            : (std::string("miniupnpc mapped ICE UDP port with external IP ") + result.external_ip);
    } else {
        result.status = "mapping_failed";
        result.detail = strupnperror(mapping_error) != nullptr
            ? strupnperror(mapping_error)
            : (std::string("UPnP mapping failed with code ") + std::to_string(mapping_error));
    }

    FreeUPNPUrls(&urls);
    freeUPNPDevlist(devlist);
    return result;
#endif
}

}  // namespace redclaw::service
