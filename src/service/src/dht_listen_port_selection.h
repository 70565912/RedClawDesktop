#pragma once

#include <string>
#include <string_view>
#include <functional>
#include <cstdint>

namespace redclaw::service::detail {

struct DhtListenPortSelection {
    int selected_port = 0;
    bool os_assigned = false;
    std::string error_detail;
    unsigned probe_attempts = 0;
};

inline constexpr unsigned kDhtPortProbeLimit = 16;
using DhtPortProbe = std::function<DhtListenPortSelection(int)>;

enum class DhtListenerState { kPending, kReady, kFailed };
class DhtListenerStartup {
public:
    explicit DhtListenerStartup(std::uint64_t started_ms) : started_ms_(started_ms) {}
    [[nodiscard]] DhtListenerState state() const { return state_; }
    void udp_ready(int port) {
        if (state_ != DhtListenerState::kPending || port <= 0 || port > 65535) return;
        port_ = port;
        state_ = DhtListenerState::kReady;
    }
    [[nodiscard]] int port() const { return port_; }
    DhtListenerState evaluate(std::uint64_t now_ms) {
        if (state_ == DhtListenerState::kPending && now_ms >= started_ms_ && now_ms - started_ms_ >= 5000)
            state_ = DhtListenerState::kFailed;
        return state_;
    }
private:
    std::uint64_t started_ms_;
    DhtListenerState state_ = DhtListenerState::kPending;
    int port_ = 0;
};
// Probe owns all candidate sockets until both protocols/families have passed.
[[nodiscard]] DhtListenPortSelection select_dht_listen_port_bounded(
    int requested_port, const DhtPortProbe& probe);

[[nodiscard]] DhtListenPortSelection select_dht_listen_port(
    int requested_port,
    std::string_view listen_address,
    std::string_view listen_ipv6_address = {}, bool enable_ipv6 = false);

}  // namespace redclaw::service::detail
