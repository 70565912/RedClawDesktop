#include "redclaw/net/net_module.h"
#include "redclaw/net/ice_candidate_diagnostics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <future>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <rtc/rtc.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "stun_server_probe.h"
namespace redclaw::net {
namespace {
constexpr std::size_t kMaxConcurrentStunProbes = 4;
constexpr std::uint32_t kStunProbeTimeoutMs = 750;

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

class ScopedWinsock final {
public:
    ScopedWinsock() {
        WSADATA data {};
        ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }

    ~ScopedWinsock() {
        if (ready_) {
            WSACleanup();
        }
    }

    [[nodiscard]] bool ready() const {
        return ready_;
    }

private:
    bool ready_ = false;
};

void close_socket(SocketHandle socket_handle) {
    closesocket(socket_handle);
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

class ScopedWinsock final {
public:
    [[nodiscard]] bool ready() const {
        return true;
    }
};

void close_socket(SocketHandle socket_handle) {
    close(socket_handle);
}
#endif

struct ParsedStunServer {
    std::string uri;
    std::string hostname;
    std::uint16_t port = 3478;
};

bool bind_probe_socket(SocketHandle socket_handle, int family, std::string_view bind_address) {
    if (bind_address.empty()) {
        return true;
    }
    if (family != AF_INET) {
        return false;
    }

    sockaddr_in local_address {};
    local_address.sin_family = AF_INET;
    local_address.sin_port = 0;
    const std::string bind_address_text(bind_address);
    if (inet_pton(AF_INET, bind_address_text.c_str(), &local_address.sin_addr) != 1) {
        return false;
    }
    return bind(
        socket_handle,
        reinterpret_cast<const sockaddr*>(&local_address),
        static_cast<int>(sizeof(local_address))) == 0;
}

bool set_probe_receive_timeout(SocketHandle socket_handle) {
#ifdef _WIN32
    const DWORD timeout = kStunProbeTimeoutMs;
    return setsockopt(
        socket_handle,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout),
        static_cast<int>(sizeof(timeout))) == 0;
#else
    timeval timeout {};
    timeout.tv_sec = static_cast<long>(kStunProbeTimeoutMs / 1000U);
    timeout.tv_usec = static_cast<long>((kStunProbeTimeoutMs % 1000U) * 1000U);
    return setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0;
#endif
}

std::array<std::uint8_t, 20> make_stun_binding_request() {
    std::array<std::uint8_t, 20> request{};
    request[1] = 0x01;
    request[4] = 0x21;
    request[5] = 0x12;
    request[6] = 0xA4;
    request[7] = 0x42;
    std::random_device random;
    for (std::size_t index = 8; index < request.size(); ++index) {
        request[index] = static_cast<std::uint8_t>(random());
    }
    return request;
}

bool is_matching_stun_binding_response(
    std::span<const std::uint8_t> response,
    const std::array<std::uint8_t, 20>& request) {
    if (response.size() < request.size()
        || response[0] != 0x01
        || response[1] != 0x01
        || !std::equal(request.begin() + 4, request.end(), response.begin() + 4)) {
        return false;
    }
    return true;
}

StunServerProbeResult probe_stun_server(
    const ParsedStunServer& server,
    std::string_view bind_address) {
    StunServerProbeResult result;
    result.server_uri = server.uri;
    const auto started_at = std::chrono::steady_clock::now();

    ScopedWinsock winsock;
    if (!winsock.ready()) {
        return result;
    }

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(server.port);
    if (getaddrinfo(server.hostname.c_str(), service.c_str(), &hints, &addresses) != 0) {
        return result;
    }

    const auto request = make_stun_binding_request();
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        SocketHandle socket_handle = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_handle == kInvalidSocket) {
            continue;
        }
        if (!set_probe_receive_timeout(socket_handle)
            || !bind_probe_socket(socket_handle, address->ai_family, bind_address)
            || connect(socket_handle, address->ai_addr, static_cast<int>(address->ai_addrlen)) != 0) {
            close_socket(socket_handle);
            continue;
        }

        const int sent = send(
            socket_handle,
            reinterpret_cast<const char*>(request.data()),
            static_cast<int>(request.size()),
            0);
        std::array<std::uint8_t, 512> response{};
        const int received = sent == static_cast<int>(request.size())
            ? recv(
                  socket_handle,
                  reinterpret_cast<char*>(response.data()),
                  static_cast<int>(response.size()),
                  0)
            : -1;
        close_socket(socket_handle);
        if (received > 0
            && is_matching_stun_binding_response(
                std::span<const std::uint8_t>(response.data(), static_cast<std::size_t>(received)),
                request)) {
            result.responded = true;
            result.response_time_ms = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_at).count());
            break;
        }
    }
    freeaddrinfo(addresses);
    return result;
}
}  // namespace

std::string choose_preferred_stun_server(
    const std::vector<std::string>& ordered_stun_server_uris,
    std::span<const StunServerProbeResult> probe_results) {
    if (ordered_stun_server_uris.empty()) {
        return {};
    }

    const auto primary_response = std::find_if(
        probe_results.begin(),
        probe_results.end(),
        [&ordered_stun_server_uris](const StunServerProbeResult& probe) {
            return probe.responded && probe.server_uri == ordered_stun_server_uris.front();
        });
    if (primary_response != probe_results.end()) {
        return ordered_stun_server_uris.front();
    }

    const StunServerProbeResult* best = nullptr;
    std::size_t best_order = ordered_stun_server_uris.size();
    for (const auto& probe : probe_results) {
        if (!probe.responded) {
            continue;
        }
        const auto ordered = std::find(
            ordered_stun_server_uris.begin(),
            ordered_stun_server_uris.end(),
            probe.server_uri);
        if (ordered == ordered_stun_server_uris.end()) {
            continue;
        }
        const std::size_t order = static_cast<std::size_t>(ordered - ordered_stun_server_uris.begin());
        if (best == nullptr
            || probe.response_time_ms < best->response_time_ms
            || (probe.response_time_ms == best->response_time_ms && order < best_order)) {
            best = &probe;
            best_order = order;
        }
    }
    return best != nullptr ? best->server_uri : ordered_stun_server_uris.front();
}

std::vector<std::string> select_ice_servers_for_gathering(
    const std::vector<std::string>& configured_servers,
    std::string_view bind_address) {
    std::vector<ParsedStunServer> stun_servers;
    stun_servers.reserve(configured_servers.size());
    for (const auto& uri : configured_servers) {
        try {
            rtc::IceServer parsed(uri);
            if (parsed.type == rtc::IceServer::Type::Stun && !parsed.hostname.empty()) {
                const std::uint16_t port = parsed.port == 0
                                               ? static_cast<std::uint16_t>(3478)
                                               : parsed.port;
                stun_servers.push_back({uri, parsed.hostname, port});
            }
        } catch (const std::exception&) {
            // Preserve invalid entries so PeerConnection returns its existing
            // validation error instead of silently changing CLI behavior.
        }
    }
    if (stun_servers.size() <= 1) {
        return configured_servers;
    }

    const std::size_t probe_count = (std::min)(stun_servers.size(), kMaxConcurrentStunProbes);
    std::vector<std::future<StunServerProbeResult>> pending;
    pending.reserve(probe_count);
    for (std::size_t index = 0; index < probe_count; ++index) {
        pending.push_back(std::async(
            std::launch::async,
            [server = stun_servers[index], bind_address = std::string(bind_address)]() {
                return probe_stun_server(server, bind_address);
            }));
    }

    std::vector<StunServerProbeResult> probe_results;
    probe_results.reserve(pending.size());
    for (auto& future : pending) {
        probe_results.push_back(future.get());
    }

    std::vector<std::string> ordered_stun_uris;
    ordered_stun_uris.reserve(stun_servers.size());
    for (const auto& server : stun_servers) {
        ordered_stun_uris.push_back(server.uri);
    }
    const std::string selected_stun = choose_preferred_stun_server(ordered_stun_uris, probe_results);

    std::vector<std::string> selected;
    selected.reserve(configured_servers.size() - stun_servers.size() + 1);
    bool selected_stun_added = false;
    for (const auto& uri : configured_servers) {
        const auto stun_it = std::find(ordered_stun_uris.begin(), ordered_stun_uris.end(), uri);
        if (stun_it == ordered_stun_uris.end()) {
            selected.push_back(uri);
        } else if (!selected_stun_added && uri == selected_stun) {
            selected.push_back(uri);
            selected_stun_added = true;
        }
    }
    return selected;
}
}  // namespace redclaw::net
