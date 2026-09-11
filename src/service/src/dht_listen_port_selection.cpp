#include "dht_listen_port_selection.h"

#include <array>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace redclaw::service::detail {

DhtListenPortSelection select_dht_listen_port_bounded(int requested_port, const DhtPortProbe& probe) {
    if (requested_port < 0 || requested_port > 65535 || !probe)
        return {.error_detail = "DHT listen port/probe is invalid"};
    const unsigned limit = requested_port == 0 ? kDhtPortProbeLimit : 1;
    DhtListenPortSelection result;
    for (unsigned attempt = 1; attempt <= limit; ++attempt) {
        result = probe(requested_port);
        result.probe_attempts = attempt;
        if (result.error_detail.empty() && result.selected_port > 0 && result.selected_port <= 65535
            && (requested_port == 0 || result.selected_port == requested_port)) {
            result.os_assigned = requested_port == 0;
            return result;
        }
    }
    result.selected_port = 0;
    result.os_assigned = false;
    if (result.error_detail.empty()) result.error_detail = "DHT joint TCP/UDP port preflight failed";
    return result;
}

namespace {
#if defined(_WIN32)
struct WinsockLease {
    WSADATA data{};
    int error = WSAStartup(MAKEWORD(2, 2), &data);
    ~WinsockLease() { if (error == 0) WSACleanup(); }
};
struct ProbeSockets {
    std::array<SOCKET, 4> values{INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET};
    ~ProbeSockets() { for (const auto value : values) if (value != INVALID_SOCKET) closesocket(value); }
};
DhtListenPortSelection probe_joint_port(int port, sockaddr_in ipv4, const sockaddr_in6* ipv6) {
    ProbeSockets sockets;
    const unsigned count = ipv6 ? 4 : 2;
    for (unsigned index = 0; index < count; ++index) {
        const bool v6 = index >= 2;
        // UDP allocates port zero first: Windows TCP allocation can choose a
        // UDP-excluded port repeatedly. Keep all probes alive until TCP and
        // every enabled address family have also passed.
        const bool tcp = index % 2 != 0;
        const auto sock = socket(v6 ? AF_INET6 : AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM,
            tcp ? IPPROTO_TCP : IPPROTO_UDP);
        sockets.values[index] = sock;
        const auto fail = [&](const char* stage) {
            return DhtListenPortSelection{.error_detail = std::string("DHT ")
                + (v6 ? "IPv6 " : "IPv4 ") + (tcp ? "TCP " : "UDP ") + stage
                + " failed (" + std::to_string(WSAGetLastError()) + ")"};
        };
        if (sock == INVALID_SOCKET) return fail("socket");
        BOOL exclusive = TRUE;
        if (setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) return fail("exclusive bind option");
        sockaddr_in6 endpoint6{};
        sockaddr* address = reinterpret_cast<sockaddr*>(&ipv4);
        int size = sizeof(ipv4);
        ipv4.sin_port = htons(static_cast<unsigned short>(port));
        if (v6) {
            DWORD only_v6 = 1;
            if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
                reinterpret_cast<const char*>(&only_v6), sizeof(only_v6)) != 0) return fail("v6-only option");
            endpoint6 = *ipv6;
            endpoint6.sin6_port = htons(static_cast<unsigned short>(port));
            address = reinterpret_cast<sockaddr*>(&endpoint6);
            size = sizeof(endpoint6);
        }
        if (bind(sock, address, size) != 0) return fail("bind");
        if (tcp && listen(sock, 1) != 0) return fail("listen");
        if (index == 0 && port == 0) {
            if (getsockname(sock, address, &size) != 0) return fail("getsockname");
            port = ntohs(ipv4.sin_port);
            if (port == 0) return {.error_detail = "DHT OS assigned an invalid port"};
        }
    }
    // The handoff to libtorrent is necessarily racy; native listener alerts remain authoritative.
    return {.selected_port = port};
}
#endif
} // namespace

DhtListenPortSelection select_dht_listen_port(int requested_port, std::string_view listen_address,
                                             std::string_view listen_ipv6_address, bool enable_ipv6) {
#if defined(_WIN32)
    WinsockLease winsock;
    if (winsock.error != 0) return {.error_detail = "DHT Winsock startup failed"};
    sockaddr_in ipv4{};
    ipv4.sin_family = AF_INET;
    ipv4.sin_addr.s_addr = htonl(INADDR_ANY);
    if (!listen_address.empty() && inet_pton(AF_INET, std::string(listen_address).c_str(), &ipv4.sin_addr) != 1)
        return {.error_detail = "DHT IPv4 listen address is invalid"};
    // Match make_libtorrent_listen_interfaces: a fixed IPv4 interface adds IPv6
    // only when an explicit IPv6 address exists; automatic interface mode uses ::.
    const bool use_ipv6 = enable_ipv6 && (listen_address.empty() || !listen_ipv6_address.empty());
    sockaddr_in6 ipv6{};
    ipv6.sin6_family = AF_INET6;
    if (use_ipv6 && !listen_ipv6_address.empty()
        && inet_pton(AF_INET6, std::string(listen_ipv6_address).c_str(), &ipv6.sin6_addr) != 1)
        return {.error_detail = "DHT IPv6 listen address is invalid"};
    return select_dht_listen_port_bounded(requested_port,
        [&](int port) { return probe_joint_port(port, ipv4, use_ipv6 ? &ipv6 : nullptr); });
#else
    (void)listen_address; (void)listen_ipv6_address; (void)enable_ipv6;
    if (requested_port < 0 || requested_port > 65535) return {.error_detail = "DHT listen port is invalid"};
    return {.selected_port = requested_port};
#endif
}
} // namespace redclaw::service::detail
