#include "redclaw/net/net_module.h"
#include "stun_server_probe.h"
#include "redclaw/net/ice_candidate_diagnostics.h"

#include <algorithm>
#include <array>
#include <cerrno>
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

namespace redclaw::net {

namespace {

constexpr std::size_t kDataChannelBufferedAmountLowThresholdBytes = 64 * 1024;

struct UdpPortReservationResult {
    bool available = false;
    int native_error = 0;
    std::string detail;
};

class FixedUdpPortReservation final {
public:
    FixedUdpPortReservation() = default;
    ~FixedUdpPortReservation() { release(); }
    FixedUdpPortReservation(const FixedUdpPortReservation&) = delete;
    FixedUdpPortReservation& operator=(const FixedUdpPortReservation&) = delete;

    [[nodiscard]] UdpPortReservationResult acquire(
        std::uint16_t port,
        std::string_view bind_address) {
        UdpPortReservationResult result;
        if (active()) {
            if (port_ == port && bind_address_ == bind_address) {
                result.available = true;
                return result;
            }
            result.detail = "a different ICE UDP port is already reserved";
            return result;
        }
#ifdef _WIN32
        WSADATA winsock_data{};
        const int startup_error = WSAStartup(MAKEWORD(2, 2), &winsock_data);
        if (startup_error != 0) {
            result.native_error = startup_error;
            result.detail = "WSAStartup failed";
            return result;
        }
        winsock_started_ = true;

        socket_handle_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_handle_ == INVALID_SOCKET) {
            result.native_error = WSAGetLastError();
            result.detail = "UDP socket creation failed";
            release();
            return result;
        }

        const BOOL exclusive = TRUE;
        if (setsockopt(
                socket_handle_,
                SOL_SOCKET,
                SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive),
                sizeof(exclusive)) == SOCKET_ERROR) {
            result.native_error = WSAGetLastError();
            result.detail = "SO_EXCLUSIVEADDRUSE failed";
            release();
            return result;
        }

        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(port);
        if (bind_address.empty()) {
            endpoint.sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (InetPtonA(AF_INET, std::string(bind_address).c_str(), &endpoint.sin_addr) != 1) {
            result.native_error = WSAEINVAL;
            result.detail = "invalid IPv4 bind address";
            release();
            return result;
        }

        if (bind(socket_handle_, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR) {
            result.native_error = WSAGetLastError();
            result.detail = "exclusive UDP bind failed";
            release();
            return result;
        }

#else
        socket_handle_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_handle_ < 0) {
            result.native_error = errno;
            result.detail = "UDP socket creation failed";
            return result;
        }

        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(port);
        if (bind_address.empty()) {
            endpoint.sin_addr.s_addr = htonl(INADDR_ANY);
        } else if (inet_pton(AF_INET, std::string(bind_address).c_str(), &endpoint.sin_addr) != 1) {
            result.native_error = EINVAL;
            result.detail = "invalid IPv4 bind address";
            release();
            return result;
        }

        if (bind(socket_handle_, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0) {
            result.native_error = errno;
            result.detail = "exclusive UDP bind failed";
            release();
            return result;
        }
#endif
        port_ = port;
        bind_address_ = bind_address;
        result.available = true;
        return result;
    }

    [[nodiscard]] bool active() const noexcept {
#ifdef _WIN32
        return socket_handle_ != INVALID_SOCKET;
#else
        return socket_handle_ >= 0;
#endif
    }

    void release() noexcept {
#ifdef _WIN32
        if (socket_handle_ != INVALID_SOCKET) {
            closesocket(socket_handle_);
            socket_handle_ = INVALID_SOCKET;
        }
        if (winsock_started_) {
            WSACleanup();
            winsock_started_ = false;
        }
#else
        if (socket_handle_ >= 0) {
            close(socket_handle_);
            socket_handle_ = -1;
        }
#endif
        port_ = 0;
        bind_address_.clear();
    }

private:
#ifdef _WIN32
    SOCKET socket_handle_ = INVALID_SOCKET;
    bool winsock_started_ = false;
#else
    int socket_handle_ = -1;
#endif
    std::uint16_t port_ = 0;
    std::string bind_address_;
};

constexpr std::size_t data_channel_index(DataChannelKind kind) {
    switch (kind) {
        case DataChannelKind::kMedia: return 0U;
        case DataChannelKind::kControl: return 1U;
        case DataChannelKind::kAgent: return 2U;
        case DataChannelKind::kNavigation: return 3U;
        case DataChannelKind::kDebugBridge: return 4U;
    }
    return 4U;
}

std::optional<DataChannelKind> data_channel_kind_from_label(std::string_view label) {
    if (label == kMediaDataChannelLabel) {
        return DataChannelKind::kMedia;
    }
    if (label == kControlDataChannelLabel) {
        return DataChannelKind::kControl;
    }
    if (label == kAgentDataChannelLabel) {
        return DataChannelKind::kAgent;
    }
    if (label == kNavigationDataChannelLabel) {
        return DataChannelKind::kNavigation;
    }
    if (label == kDebugBridgeDataChannelLabel) {
        return DataChannelKind::kDebugBridge;
    }
    return std::nullopt;
}

std::string_view data_channel_label(DataChannelKind kind) {
    switch (kind) {
        case DataChannelKind::kMedia: return kMediaDataChannelLabel;
        case DataChannelKind::kControl: return kControlDataChannelLabel;
        case DataChannelKind::kAgent: return kAgentDataChannelLabel;
        case DataChannelKind::kNavigation: return kNavigationDataChannelLabel;
        case DataChannelKind::kDebugBridge: return kDebugBridgeDataChannelLabel;
    }
    return {};
}

rtc::DataChannelInit make_data_channel_init(DataChannelKind kind) {
    const DataChannelDeliveryPolicy policy = data_channel_delivery_policy(kind);
    rtc::DataChannelInit init;
    init.reliability.unordered = !policy.ordered;
    if (!policy.reliable) {
        init.reliability.type = rtc::Reliability::Type::Rexmit;
        init.reliability.rexmit = static_cast<int>(policy.max_retransmits);
    }
    return init;
}




IceConnectionState to_ice_connection_state(rtc::PeerConnection::State state) {
    switch (state) {
    case rtc::PeerConnection::State::New:
        return IceConnectionState::kNew;
    case rtc::PeerConnection::State::Connecting:
        return IceConnectionState::kConnecting;
    case rtc::PeerConnection::State::Connected:
        return IceConnectionState::kConnected;
    case rtc::PeerConnection::State::Disconnected:
        return IceConnectionState::kDisconnected;
    case rtc::PeerConnection::State::Failed:
        return IceConnectionState::kFailed;
    case rtc::PeerConnection::State::Closed:
        return IceConnectionState::kClosed;
    }

    return IceConnectionState::kFailed;
}

IceGatheringState to_ice_gathering_state(rtc::PeerConnection::GatheringState state) {
    switch (state) {
    case rtc::PeerConnection::GatheringState::New:
        return IceGatheringState::kNew;
    case rtc::PeerConnection::GatheringState::InProgress:
        return IceGatheringState::kInProgress;
    case rtc::PeerConnection::GatheringState::Complete:
        return IceGatheringState::kComplete;
    }
    return IceGatheringState::kNew;
}

void assign_error(std::string_view error, std::string* error_detail) {
    if (error_detail != nullptr) {
        *error_detail = std::string(error);
    }
}

bool has_control_chars(const std::string& value) {
    for (const unsigned char ch : value) {
        if (std::iscntrl(ch) != 0) {
            return true;
        }
    }
    return false;
}

bool is_valid_mid(const std::string& mid) {
    if (mid.empty() || mid.size() > 64) {
        return false;
    }

    for (const unsigned char ch : mid) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_') {
            continue;
        }
        return false;
    }

    return true;
}

bool has_candidate_prefix(const std::string& candidate_sdp) {
    constexpr std::string_view kCandidatePrefix = "candidate:";
    constexpr std::string_view kAttributeCandidatePrefix = "a=candidate:";

    return candidate_sdp.rfind(std::string(kCandidatePrefix), 0) == 0
        || candidate_sdp.rfind(std::string(kAttributeCandidatePrefix), 0) == 0;
}

bool has_min_candidate_tokens(const std::string& candidate_sdp) {
    std::size_t token_count = 0;
    bool in_token = false;

    for (const unsigned char ch : candidate_sdp) {
        if (std::isspace(ch) != 0) {
            in_token = false;
            continue;
        }

        if (!in_token) {
            ++token_count;
            in_token = true;
        }
    }

    return token_count >= 8;
}

bool looks_like_candidate_sdp(const std::string& candidate_sdp) {
    if (candidate_sdp.empty() || has_control_chars(candidate_sdp)) {
        return false;
    }

    if (!has_candidate_prefix(candidate_sdp)) {
        return false;
    }

    if (candidate_sdp.find(" typ ") == std::string::npos) {
        return false;
    }

    return has_min_candidate_tokens(candidate_sdp);
}

}  // namespace

DataChannelDeliveryPolicy data_channel_delivery_policy(DataChannelKind kind) {
    if (kind == DataChannelKind::kMedia) {
        return DataChannelDeliveryPolicy{
            .reliable = false,
            .ordered = true,
            .max_retransmits = 0,
        };
    }
    if (kind == DataChannelKind::kNavigation) {
        return DataChannelDeliveryPolicy{
            .reliable = false,
            .ordered = false,
            .max_retransmits = 0,
        };
    }
    return DataChannelDeliveryPolicy{};
}


std::string_view ice_check_kind_name(IceCheckKind kind) {
    static constexpr std::array<std::string_view, kIceCheckKindCount> names = {
        "request_tx", "response_tx", "request_rx", "response_rx", "validation_failed",
        "transaction_unmatched", "check_timeout", "nomination_tx", "nomination_rx",
        "send_failed", "credentials_missing", "socket_rx", "socket_rx_peer", "socket_rx_server",
        "socket_rx_unknown", "socket_io_failed", "non_stun_preselection", "stun_parse_failed",
        "state_ignored", "remote_address_unmatched", "stun_server_response", "peer_stun_parsed",
        "local_candidate_socket_match", "local_candidate_socket_mismatch",
        "send_target_match", "send_target_mismatch", "socket_ready", "udp_ignored_connreset",
        "udp_ignored_netreset", "udp_ignored_connrefused", "udp_error_observation_ready"};
    const auto index = static_cast<std::size_t>(kind);
    return index < names.size() ? names[index] : "unknown";
}

class IceConnectivityWrapper::Impl : public std::enable_shared_from_this<Impl> {
    friend class IceConnectivityWrapper;

    struct ChannelState {
        std::shared_ptr<rtc::DataChannel> channel;
        std::uint64_t generation = 0;
        bool open = false;
        bool send_blocked = false;
        std::size_t buffered_amount_low_threshold = kDataChannelBufferedAmountLowThresholdBytes;
    };
    struct PeerSnapshot {
        std::shared_ptr<rtc::PeerConnection> peer;
        std::uint64_t generation = 0;
        bool offer = false;
    };
    mutable std::mutex mutex_;
    std::condition_variable drained_;
    std::size_t callbacks_in_flight_ = 0;
    bool shutting_down_ = false;
    inline static thread_local Impl* callback_owner_ = nullptr;
    std::shared_ptr<rtc::PeerConnection> peer_connection_;
    std::uint64_t peer_generation_ = 0;
    std::array<ChannelState, 5> data_channels_;
    bool initiates_offer_ = false;
    bool remote_description_ready_ = false;
    IceConnectionState state_ = IceConnectionState::kNew;
    IceGatheringState gathering_state_ = IceGatheringState::kNew;
    TransportDiagnostics diagnostics_;
    IceCandidateDiagnostics candidate_diagnostics_;
    FixedUdpPortReservation fixed_udp_port_reservation_;
    std::array<std::uint64_t, kIceCheckKindCount> ice_check_last_event_ms_{};
    ConnectionStateChangedCallback state_callback_;
    GatheringStateChangedCallback gathering_state_callback_;
    LocalCandidateCallback candidate_callback_;
    LocalDescriptionCallback description_callback_;
    DataChannelOpenCallback data_channel_open_callback_;
    DataChannelClosedCallback data_channel_closed_callback_;
    DataChannelMessageCallback data_channel_message_callback_;
    DataChannelBinaryMessageCallback data_channel_binary_message_callback_;
    DataChannelWritableCallback data_channel_writable_callback_;

    bool current_locked(std::uint64_t peer, std::optional<DataChannelKind> kind,
                        std::uint64_t channel) const {
        return !shutting_down_ && peer_generation_ == peer
            && (!kind || data_channels_[data_channel_index(*kind)].generation == channel);
    }
    void diagnostic_locked(TransportDiagnosticLayer layer, int native_state,
                           bool failure, std::string reason,
                           std::optional<DataChannelKind> kind = {}, std::uint64_t generation = 0) {
        TransportDiagnosticEvent event;
        event.sequence = ++diagnostics_.event_sequence;
        event.steady_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        event.peer_generation = peer_generation_;
        event.channel_generation = generation;
        event.layer = layer;
        event.channel = kind;
        event.native_state = native_state;
        event.failure = failure;
        event.reason = std::move(reason); // Only local, fixed categories; never raw native secrets.
        if (failure && !diagnostics_.first_failure) diagnostics_.first_failure = event;
        if (diagnostics_.recent_events.size() == 64) diagnostics_.recent_events.erase(diagnostics_.recent_events.begin());
        if (layer == TransportDiagnosticLayer::kIceCandidate && diagnostics_.candidate_events.size() < 64)
            diagnostics_.candidate_events.push_back(event);
        diagnostics_.recent_events.push_back(std::move(event));
    }

    void candidate_observations_locked(const std::vector<IceCandidateObservation>& rows) {
        for (const auto& row : rows)
            diagnostic_locked(TransportDiagnosticLayer::kIceCandidate, 0, false, row.diagnostic_reason());
    }

    bool reserve_fixed_udp_port(
        std::uint16_t port,
        std::string_view bind_address,
        std::string* error) {
        if (port == 0) {
            assign_error("ICE UDP port must be between 1 and 65535", error);
            return false;
        }
        std::lock_guard lock(mutex_);
        if (shutting_down_) {
            assign_error("wrapper shut down", error);
            return false;
        }
        const auto reservation = fixed_udp_port_reservation_.acquire(port, bind_address);
        if (!reservation.available) {
            assign_error(
                "ICE UDP port " + std::to_string(port)
                    + " is unavailable: " + reservation.detail
                    + " (native_error=" + std::to_string(reservation.native_error) + ")",
                error);
            return false;
        }
        assign_error({}, error);
        return true;
    }

    void release_fixed_udp_port_reservation() {
        std::lock_guard lock(mutex_);
        fixed_udp_port_reservation_.release();
    }

    // Holds only the lifetime lease across the application callback, not mutex_.
    // Callback registration and state transition happen under the short lock.
    template<class Fn>
    auto guarded(std::uint64_t peer, std::optional<DataChannelKind> kind,
                 std::uint64_t channel, Fn fn) {
        return [weak = weak_from_this(), peer, kind, channel, fn = std::move(fn)](auto... args) {
            auto self = weak.lock();
            if (!self) return;
            std::unique_lock lock(self->mutex_);
            if (!self->current_locked(peer, kind, channel)) {
                ++self->diagnostics_.ignored_stale_callbacks;
                return;
            }
            ++self->callbacks_in_flight_;
            Impl* previous = callback_owner_;
            callback_owner_ = self.get();
            try {
                fn(*self, lock, std::move(args)...);
            } catch (...) {
                if (!lock.owns_lock()) lock.lock();
                ++self->diagnostics_.callback_failures;
                self->diagnostic_locked(TransportDiagnosticLayer::kPeerConnection, 0, true, "application_callback_exception");
            }
            if (!lock.owns_lock()) lock.lock();
            callback_owner_ = previous;
            --self->callbacks_in_flight_;
            lock.unlock();
            self->drained_.notify_all();
        };
    }
    PeerSnapshot peer_snapshot() const {
        std::lock_guard lock(mutex_);
        return {peer_connection_, peer_generation_, initiates_offer_};
    }
    ChannelState channel_snapshot(DataChannelKind kind) const {
        std::lock_guard lock(mutex_);
        return data_channels_[data_channel_index(kind)];
    }
    bool peer_is_current(const PeerSnapshot& snapshot) const {
        std::lock_guard lock(mutex_);
        return !shutting_down_ && peer_generation_ == snapshot.generation && peer_connection_ == snapshot.peer;
    }
    template<class Fn>
    bool peer_operation(Fn fn, std::string* error) {
        const auto snapshot = peer_snapshot();
        if (!snapshot.peer) { assign_error("peer connection not initialized", error); return false; }
        try {
            fn(snapshot);
            if (!peer_is_current(snapshot)) { assign_error("peer generation changed", error); return false; }
            assign_error({}, error);
            return true;
        } catch (...) {
            assign_error("native peer operation failed; details withheld", error);
            return false;
        }
    }
    template<class Fn>
    bool send(DataChannelKind kind, Fn fn, std::string* error, DataChannelSendOutcome* outcome) {
        const auto snapshot = channel_snapshot(kind);
        if (!snapshot.channel || !snapshot.open) {
            assign_error("data channel is not open", error);
            return false;
        }
        try {
            const bool immediate = fn(*snapshot.channel);
            const auto buffered = snapshot.channel->bufferedAmount();
            {
                std::lock_guard lock(mutex_);
                auto& state = data_channels_[data_channel_index(kind)];
                if (shutting_down_ || state.generation != snapshot.generation || state.channel != snapshot.channel) {
                    assign_error("channel generation changed", error);
                    return false;
                }
                state.send_blocked = buffered > 0 && (!immediate || buffered >= state.buffered_amount_low_threshold);
            }
            if (outcome) {
                outcome->disposition = immediate ? DataChannelSendDisposition::kImmediate : DataChannelSendDisposition::kAcceptedQueued;
                outcome->buffered_amount = buffered;
            }
            assign_error({}, error);
            return true;
        } catch (...) {
            assign_error("data channel send failed; native details withheld", error);
            return false;
        }
    }
    void bind_data_channel(std::shared_ptr<rtc::DataChannel> channel, std::uint64_t peer) {
        if (!channel) return;
        const auto kind = data_channel_kind_from_label(channel->label());
        if (!kind) { channel->close(); return; }
        std::shared_ptr<rtc::DataChannel> old;
        std::uint64_t generation = 0;
        std::size_t threshold = 0;
        {
            std::lock_guard lock(mutex_);
            auto& state = data_channels_[data_channel_index(*kind)];
            if (current_locked(peer, {}, 0) && (!state.channel || !state.open)) {
                old = std::move(state.channel);
                state.channel = channel;
                generation = ++state.generation;
                state.open = false;
                state.send_blocked = false;
                threshold = state.buffered_amount_low_threshold;
            }
        }
        if (!generation) { channel->close(); return; }
        if (old && old != channel) old->close();
        channel->setBufferedAmountLowThreshold(threshold);
        channel->onOpen(guarded(peer, kind, generation, [kind](Impl& s, auto& lock) {
            auto& state = s.data_channels_[data_channel_index(*kind)];
            state.open = true; state.send_blocked = false;
            s.diagnostic_locked(TransportDiagnosticLayer::kDataChannel, 1, false, "open", kind, state.generation);
            auto cb = s.data_channel_open_callback_;
            lock.unlock();
            if (cb) cb(*kind);
        }));
        channel->onClosed(guarded(peer, kind, generation, [kind](Impl& s, auto& lock) {
            auto& state = s.data_channels_[data_channel_index(*kind)];
            state.open = false; state.send_blocked = false;
            s.diagnostic_locked(TransportDiagnosticLayer::kDataChannel, 0, true, "native_close_cause_unknown", kind, state.generation);
            auto cb = s.data_channel_closed_callback_;
            lock.unlock();
            if (cb) cb(*kind);
        }));
        channel->onError(guarded(peer, kind, generation, [kind](Impl& s, auto&, std::string) {
            const auto& state = s.data_channels_[data_channel_index(*kind)];
            s.diagnostic_locked(TransportDiagnosticLayer::kDataChannel, -1, true,
                "native_channel_error_details_withheld_dtls_sctp_unknown", kind, state.generation);
        }));
        auto writable = guarded(peer, kind, generation, [kind](Impl& s, auto& lock) {
            s.data_channels_[data_channel_index(*kind)].send_blocked = false;
            auto cb = s.data_channel_writable_callback_;
            lock.unlock();
            if (cb) cb(*kind);
        });
        channel->onBufferedAmountLow(writable);
        channel->onAvailable(std::move(writable));
        channel->onMessage(guarded(peer, kind, generation, [kind](Impl& s, auto& lock, rtc::message_variant data) {
            auto text_cb = s.data_channel_message_callback_;
            auto binary_cb = s.data_channel_binary_message_callback_;
            lock.unlock();
            if (auto text = std::get_if<rtc::string>(&data)) {
                if (text_cb) text_cb(*kind, *text);
            } else if (auto binary = std::get_if<rtc::binary>(&data); binary && binary_cb) {
                binary_cb(*kind, {reinterpret_cast<const std::uint8_t*>(binary->data()), binary->size()});
            }
        }));
    }

public:
    bool start_gathering(const IceGatheringConfig& config, std::string* error) {
        if (config.initial_check_window_ms > 120000) {
            assign_error("initial ICE check window exceeds 120000 ms", error); return false;
        }
        if (callback_owner_ == this) {
            assign_error("gathering restart must run on the owner thread", error); return false;
        }
        if (!retire_and_drain()) {
            assign_error("gathering retirement requires owner thread", error); return false;
        }
        if (config.port_range_begin != 0
            && config.port_range_begin == config.port_range_end
            && !reserve_fixed_udp_port(config.port_range_begin, config.bind_address, error)) {
            return false;
        }
        std::uint64_t generation;
        {
            std::unique_lock lock(mutex_);
            if (shutting_down_) { assign_error("wrapper shut down", error); return false; }
            generation = ++peer_generation_;
            initiates_offer_ = config.initiate_offer;
            diagnostics_.first_failure.reset();
            diagnostics_.ice_check_generation = generation;
            diagnostics_.ice_check_totals.fill(0);
            diagnostics_.candidate_events.clear();
            candidate_diagnostics_.reset();
            ice_check_last_event_ms_.fill(0);
        }
        rtc::Configuration native;
        native.disableAutoNegotiation = true;
        native.enableIceTcp = config.enable_ice_tcp;
        native.initialIceCheckWindowMs = config.initial_check_window_ms;
        native.portRangeBegin = config.port_range_begin;
        native.portRangeEnd = config.port_range_end;
        if (!config.bind_address.empty()) native.bindAddress = config.bind_address;
        try {
            for (const auto& server : select_ice_servers_for_gathering(config.ice_servers, config.bind_address)) {
                native.iceServers.emplace_back(server);
            }
            auto peer = std::make_shared<rtc::PeerConnection>(native);
            bool accepted = false;
            {
                std::lock_guard lock(mutex_);
                accepted = current_locked(generation, {}, 0);
                if (accepted) peer_connection_ = peer;
            }
            if (!accepted) { peer->close(); assign_error("gathering superseded", error); return false; }
            peer->onIceCheck(guarded(generation, {}, 0, [](Impl& s, auto&, rtc::IceCheckEvent check) {
                const auto index = static_cast<std::size_t>(check.kind);
                if (index >= kIceCheckKindCount) return;
                const auto total = ++s.diagnostics_.ice_check_totals[index];
                // Count every early receive event, but bound new text/ring diagnostics to
                // one event per kind per second. Established media has no native callbacks.
                if (index >= static_cast<std::size_t>(IceCheckKind::kSocketRx)) {
                    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                    if (total != 1 && now - s.ice_check_last_event_ms_[index] < 1000) return;
                    s.ice_check_last_event_ms_[index] = now;
                }
                // No packet contents. This callback never reenters the native agent.
                // Recoverable check failures are not the transport's first terminal failure.
                s.diagnostic_locked(TransportDiagnosticLayer::kIceCheck, static_cast<int>(index),
                    false, std::string(ice_check_kind_name(static_cast<IceCheckKind>(index))));
                auto& event = s.diagnostics_.recent_events.back();
                event.pair_id = check.pairId;
                event.local_candidate_type = check.localType;
                event.remote_candidate_type = check.remoteType;
                event.occurrences = total;
            }));
            peer->onStateChange(guarded(generation, {}, 0, [](Impl& s, auto& lock, rtc::PeerConnection::State state) {
                s.state_ = to_ice_connection_state(state); // Preserve existing aggregate recovery policy.
                s.diagnostic_locked(TransportDiagnosticLayer::kPeerConnection, static_cast<int>(state),
                    state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Disconnected
                        || state == rtc::PeerConnection::State::Closed, "native_peer_state_dtls_sctp_unknown");
                auto cb = s.state_callback_; const auto current = s.state_;
                lock.unlock(); if (cb) cb(current);
            }));
            peer->onIceStateChange(guarded(generation, {}, 0, [](Impl& s, auto&, rtc::PeerConnection::IceState state) {
                s.diagnostic_locked(TransportDiagnosticLayer::kIce, static_cast<int>(state),
                    state == rtc::PeerConnection::IceState::Failed || state == rtc::PeerConnection::IceState::Disconnected
                        || state == rtc::PeerConnection::IceState::Closed, "native_ice_state");
            }));
            peer->onGatheringStateChange(guarded(generation, {}, 0, [](Impl& s, auto& lock, rtc::PeerConnection::GatheringState state) {
                s.gathering_state_ = to_ice_gathering_state(state);
                auto cb = s.gathering_state_callback_; const auto current = s.gathering_state_;
                lock.unlock(); if (cb) cb(current);
            }));
            peer->onLocalCandidate(guarded(generation, {}, 0, [](Impl& s, auto& lock, rtc::Candidate candidate) {
                const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
                s.candidate_observations_locked(s.candidate_diagnostics_.observe(true, candidate.candidate(), candidate.mid(), now));
                auto cb = s.candidate_callback_;
                lock.unlock(); if (cb) cb(candidate.candidate(), candidate.mid());
            }));
            peer->onLocalDescription(guarded(generation, {}, 0, [](Impl& s, auto& lock, rtc::Description description) {
                s.candidate_observations_locked(s.candidate_diagnostics_.set_description(true, std::string(description)));
                auto cb = s.description_callback_;
                lock.unlock(); if (cb) cb(std::string(description), description.type() == rtc::Description::Type::Offer);
            }));
            peer->onDataChannel(guarded(generation, {}, 0, [generation](Impl& s, auto& lock, std::shared_ptr<rtc::DataChannel> channel) {
                lock.unlock(); s.bind_data_channel(std::move(channel), generation);
            }));
            guarded(generation, {}, 0, [](Impl& s, auto& lock) {
                s.state_ = IceConnectionState::kGathering;
                auto cb = s.state_callback_;
                lock.unlock(); if (cb) cb(IceConnectionState::kGathering);
            })();
            if (config.initiate_offer) {
                std::array<bool, 5> created{};
                bool negotiated = false;
                for (auto kind : config.data_channels) {
                    const auto index = data_channel_index(kind);
                    const auto label = data_channel_label(kind);
                    if (label.empty() || created[index]) continue;
                    created[index] = true;
                    if (!peer_is_current({peer, generation, true})) {
                        assign_error("gathering superseded", error); return false;
                    }
                    bind_data_channel(peer->createDataChannel(std::string(label), make_data_channel_init(kind)), generation);
                    negotiated = true;
                }
                if (negotiated && peer_is_current({peer, generation, true})) {
                    release_fixed_udp_port_reservation();
                    peer->setLocalDescription(rtc::Description::Type::Offer);
                }
            }
            if (!peer_is_current({peer, generation, config.initiate_offer})) {
                assign_error("gathering superseded", error); return false;
            }
            assign_error({}, error);
            return true;
        } catch (...) {
            std::lock_guard lock(mutex_);
            if (peer_generation_ == generation) {
                state_ = IceConnectionState::kFailed;
                diagnostic_locked(TransportDiagnosticLayer::kPeerConnection, -1, true, "native_gathering_exception");
            }
            assign_error("failed to initialize peer gathering; native details withheld", error);
            return false;
        }
    }
    RemoteCandidateApplyOutcome apply_remote_candidate(const std::string& candidate,
                                                       const std::string& mid, std::string* error) {
        // Recovery must not parse exception text: redaction previously hid
        // "without ICE transport" from the runtime's deferral heuristic. Only
        // our successful remote-description state proves readiness; a closed
        // peer or unexpected native exception is never a transient wait.
        using Outcome = RemoteCandidateApplyOutcome;
        const auto result = [&](Outcome outcome) {
            const char* detail = "candidate native operation failed; details withheld";
            switch (outcome) {
            case Outcome::kApplied: detail = ""; break;
            case Outcome::kNotReady: detail = "candidate remote description not ready"; break;
            case Outcome::kClosed: detail = "candidate peer closed or failed"; break;
            case Outcome::kSuperseded: detail = "candidate peer superseded"; break;
            case Outcome::kInvalid: detail = "invalid candidate or mid format"; break;
            case Outcome::kNativeFailure: break;
            }
            assign_error(detail, error);
            return outcome;
        };
        if (candidate.empty() || !looks_like_candidate_sdp(candidate) || !is_valid_mid(mid))
            return result(Outcome::kInvalid);
        PeerSnapshot s;
        {
            std::lock_guard lock(mutex_);
            if (shutting_down_ || state_ == IceConnectionState::kClosed || state_ == IceConnectionState::kFailed)
                return result(Outcome::kClosed);
            if (!peer_connection_ || !remote_description_ready_) return result(Outcome::kNotReady);
            s = {peer_connection_, peer_generation_, initiates_offer_};
        }
        try {
            s.peer->addRemoteCandidate(rtc::Candidate(candidate, mid));
            std::lock_guard lock(mutex_);
            if (!current_locked(s.generation, {}, 0)) return result(Outcome::kSuperseded);
            const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            candidate_observations_locked(candidate_diagnostics_.observe(false, candidate, mid, now));
            return result(Outcome::kApplied);
        } catch (...) {
            std::lock_guard lock(mutex_);
            if (!current_locked(s.generation, {}, 0)) return result(Outcome::kSuperseded);
            if (state_ == IceConnectionState::kClosed || state_ == IceConnectionState::kFailed)
                return result(Outcome::kClosed);
            // An unexpected native failure is NOT proof of temporary readiness.
            return result(Outcome::kNativeFailure);
        }
    }
    bool apply_remote_description(const std::string& sdp, bool offer, std::string* error, bool auto_answer) {
        if (sdp.empty()) { assign_error("remote sdp must be non-empty", error); return false; }
        return peer_operation([&](const PeerSnapshot& s) {
            s.peer->setRemoteDescription(rtc::Description(sdp, offer ? rtc::Description::Type::Offer : rtc::Description::Type::Answer));
            {
                std::lock_guard lock(mutex_);
                if (current_locked(s.generation, {}, 0)) {
                    remote_description_ready_ = true;
                    candidate_observations_locked(candidate_diagnostics_.set_description(false, sdp));
                }
            }
            if (offer && auto_answer && peer_is_current(s)) {
                release_fixed_udp_port_reservation();
                s.peer->setLocalDescription(rtc::Description::Type::Answer);
            }
        }, error);
    }
    bool emit_local_description(std::string* error) {
        return peer_operation([this](const PeerSnapshot& s) {
            release_fixed_udp_port_reservation();
            s.peer->setLocalDescription(s.offer ? rtc::Description::Type::Offer : rtc::Description::Type::Answer);
        }, error);
    }
    void set_state_callback(ConnectionStateChangedCallback cb) { std::lock_guard lock(mutex_); if (!shutting_down_) state_callback_ = std::move(cb); }
    void set_gathering_state_callback(GatheringStateChangedCallback cb) { std::lock_guard lock(mutex_); if (!shutting_down_) gathering_state_callback_ = std::move(cb); }
    void set_candidate_callback(LocalCandidateCallback cb) { std::lock_guard lock(mutex_); if (!shutting_down_) candidate_callback_ = std::move(cb); }
    void set_description_callback(LocalDescriptionCallback cb) { std::lock_guard lock(mutex_); if (!shutting_down_) description_callback_ = std::move(cb); }
    void set_data_channel_open_callback(DataChannelOpenCallback cb) { std::lock_guard lock(mutex_); if (!shutting_down_) data_channel_open_callback_ = std::move(cb); }
    void set_data_channel_closed_callback(DataChannelClosedCallback cb) { std::lock_guard lock(mutex_); if (!shutting_down_) data_channel_closed_callback_ = std::move(cb); }
    void set_data_channel_message_callback(DataChannelMessageCallback cb) { std::lock_guard lock(mutex_); if (!shutting_down_) data_channel_message_callback_ = std::move(cb); }
    void set_data_channel_binary_message_callback(DataChannelBinaryMessageCallback cb) { std::lock_guard lock(mutex_); if (!shutting_down_) data_channel_binary_message_callback_ = std::move(cb); }
    void set_data_channel_writable_callback(DataChannelWritableCallback cb) { std::lock_guard lock(mutex_); if (!shutting_down_) data_channel_writable_callback_ = std::move(cb); }
    bool send_data_channel_message(DataChannelKind kind, const std::string& text, std::string* error) {
        return send(kind, [&](rtc::DataChannel& channel) { return channel.send(text); }, error, nullptr);
    }
    bool send_data_channel_binary_message(DataChannelKind kind, std::span<const std::uint8_t> bytes,
                                          std::string* error, DataChannelSendOutcome* outcome) {
        return send(kind, [&](rtc::DataChannel& channel) {
            return channel.send(reinterpret_cast<const rtc::byte*>(bytes.data()), bytes.size());
        }, error, outcome);
    }
    bool get_data_channel_transport_stats(DataChannelKind kind, DataChannelTransportStats* stats, std::string* error) const {
        if (!stats) { assign_error("stats pointer must be non-null", error); return false; }
        const auto snapshot = channel_snapshot(kind);
        *stats = {};
        stats->buffered_amount_low_threshold = snapshot.buffered_amount_low_threshold;
        if (!snapshot.channel) { assign_error({}, error); return true; }
        try {
            stats->available = true; stats->open = snapshot.open;
            stats->buffered_amount = snapshot.channel->bufferedAmount();
            stats->available_amount = snapshot.channel->availableAmount();
            stats->max_message_size = snapshot.channel->maxMessageSize();
            stats->send_blocked = stats->buffered_amount > 0 && (snapshot.send_blocked
                || stats->buffered_amount >= snapshot.buffered_amount_low_threshold);
            {
                std::lock_guard lock(mutex_);
                const auto& state = data_channels_[data_channel_index(kind)];
                if (state.generation != snapshot.generation || state.channel != snapshot.channel) {
                    *stats = {};
                    assign_error("channel generation changed", error); return false;
                }
            }
            assign_error({}, error); return true;
        } catch (...) { assign_error("failed to read native channel stats", error); return false; }
    }
    bool set_data_channel_buffered_amount_low_threshold(DataChannelKind kind, std::size_t threshold, std::string* error) {
        std::shared_ptr<rtc::DataChannel> channel;
        {
            std::lock_guard lock(mutex_);
            auto& state = data_channels_[data_channel_index(kind)];
            state.buffered_amount_low_threshold = threshold;
            channel = state.channel;
        }
        try { if (channel) channel->setBufferedAmountLowThreshold(threshold); }
        catch (...) { assign_error("failed to set native channel threshold", error); return false; }
        assign_error({}, error); return true;
    }
    bool close_data_channel(DataChannelKind kind, std::string* error) {
        auto snapshot = channel_snapshot(kind);
        if (!snapshot.channel) { assign_error("data channel is not available", error); return false; }
        {
            std::lock_guard lock(mutex_);
            if (data_channels_[data_channel_index(kind)].generation == snapshot.generation) {
                diagnostic_locked(TransportDiagnosticLayer::kLocalClose, 0, false,
                    "explicit_channel_close", kind, snapshot.generation);
            }
        }
        try { snapshot.channel->close(); }
        catch (...) { assign_error("native channel close failed", error); return false; }
        assign_error({}, error); return true;
    }
    bool ensure_data_channel(DataChannelKind kind, std::string* error) {
        if (kind != DataChannelKind::kAgent && kind != DataChannelKind::kNavigation && kind != DataChannelKind::kDebugBridge) {
            assign_error("only an optional agent, navigation, or debug bridge data channel can be rebuilt independently", error); return false;
        }
        const auto peer = peer_snapshot();
        if (!peer.peer || !peer.offer) { assign_error("only an initialized offer initiator can create an optional channel", error); return false; }
        auto channel = channel_snapshot(kind);
        if (channel.open) { assign_error({}, error); return true; }
        return peer_operation([&](const PeerSnapshot& s) {
            if (!s.offer || s.generation != peer.generation) throw std::runtime_error("optional channel owner changed");
            bind_data_channel(s.peer->createDataChannel(std::string(data_channel_label(kind)), make_data_channel_init(kind)), s.generation);
            if (peer_is_current(s)) s.peer->setLocalDescription(rtc::Description::Type::Offer);
        }, error);
    }
    IceConnectionState state() const { std::lock_guard lock(mutex_); return state_; }
    TransportDiagnostics diagnostics() const { std::lock_guard lock(mutex_); return diagnostics_; }
    void close() {
        if (callback_owner_ == this) {
            // The owner will close the native handles. A library callback may
            // only invalidate them, never synchronously tear down its own stack.
            std::lock_guard lock(mutex_);
            diagnostic_locked(TransportDiagnosticLayer::kLocalClose, static_cast<int>(state_), false, "deferred_callback_close");
            ++peer_generation_;
            for (auto& state : data_channels_) { ++state.generation; state.open = false; state.send_blocked = false; }
            state_ = IceConnectionState::kClosed;
            remote_description_ready_ = false;
            candidate_diagnostics_.reset();
            return;
        }
        std::shared_ptr<rtc::PeerConnection> peer;
        std::array<std::shared_ptr<rtc::DataChannel>, 5> channels;
        {
            std::lock_guard lock(mutex_);
            if (peer_connection_) diagnostic_locked(TransportDiagnosticLayer::kLocalClose,
                static_cast<int>(state_), false, shutting_down_ ? "owner_shutdown" : "explicit_peer_close");
            ++peer_generation_;
            peer = std::move(peer_connection_);
            for (std::size_t i = 0; i < data_channels_.size(); ++i) {
                auto& state = data_channels_[i];
                channels[i] = std::move(state.channel);
                ++state.generation;
                state.open = false; state.send_blocked = false;
            }
            state_ = IceConnectionState::kClosed;
            gathering_state_ = IceGatheringState::kNew;
            initiates_offer_ = false;
            remote_description_ready_ = false;
            candidate_diagnostics_.reset();
        }
        // Native destruction can synchronously call back. Never hold mutex_.
        for (auto& channel : channels) if (channel) { try { channel->close(); } catch (...) {} }
        if (peer) { try { peer->close(); } catch (...) {} }
    }
    bool retire_and_drain() {
        if (callback_owner_ == this) return false;
        close();
        std::unique_lock lock(mutex_);
        drained_.wait(lock, [&] { return callbacks_in_flight_ == 0; });
        return !shutting_down_;
    }
    bool shutdown() {
        {
            std::lock_guard lock(mutex_);
            shutting_down_ = true;
            state_callback_ = {};
            gathering_state_callback_ = {};
            candidate_callback_ = {};
            description_callback_ = {};
            data_channel_open_callback_ = {};
            data_channel_closed_callback_ = {};
            data_channel_message_callback_ = {};
            data_channel_binary_message_callback_ = {};
            data_channel_writable_callback_ = {};
        }
        close();
        release_fixed_udp_port_reservation();
        if (callback_owner_ == this) return false; // Owner must finish the drain.
        std::unique_lock lock(mutex_);
        drained_.wait(lock, [&] { return callbacks_in_flight_ == 0; });
        return true;
    }
};

IceConnectivityWrapper::IceConnectivityWrapper()
    : impl_(std::make_shared<Impl>()) {}

IceConnectivityWrapper::~IceConnectivityWrapper() { (void)impl_->shutdown(); }

bool IceConnectivityWrapper::reserveFixedUdpPort(
    std::uint16_t port,
    const std::string& bind_address,
    std::string* error_detail) {
    return impl_->reserve_fixed_udp_port(port, bind_address, error_detail);
}

bool IceConnectivityWrapper::shutdown() { return impl_->shutdown(); }
bool IceConnectivityWrapper::retireAndDrain() { return impl_->retire_and_drain(); }

TransportDiagnostics IceConnectivityWrapper::diagnostics() const { return impl_->diagnostics(); }

bool IceConnectivityWrapper::startGathering(const IceGatheringConfig& config, std::string* error_detail) {
    return impl_->start_gathering(config, error_detail);
}

bool IceConnectivityWrapper::applyRemoteCandidate(
    const std::string& candidate_sdp,
    const std::string& mid,
    std::string* error_detail) {
    return tryApplyRemoteCandidate(candidate_sdp, mid, error_detail) == RemoteCandidateApplyOutcome::kApplied;
}

RemoteCandidateApplyOutcome IceConnectivityWrapper::tryApplyRemoteCandidate(
    const std::string& candidate_sdp, const std::string& mid, std::string* error_detail) {
    return impl_->apply_remote_candidate(candidate_sdp, mid, error_detail);
}

bool IceConnectivityWrapper::applyRemoteDescription(
    const std::string& sdp,
    bool is_offer,
    std::string* error_detail,
    bool auto_answer) {
    return impl_->apply_remote_description(sdp, is_offer, error_detail, auto_answer);
}

bool IceConnectivityWrapper::emitLocalDescription(std::string* error_detail) {
    return impl_->emit_local_description(error_detail);
}

void IceConnectivityWrapper::onConnectionStateChanged(ConnectionStateChangedCallback callback) {
    impl_->set_state_callback(std::move(callback));
}

void IceConnectivityWrapper::onGatheringStateChanged(GatheringStateChangedCallback callback) {
    impl_->set_gathering_state_callback(std::move(callback));
}

void IceConnectivityWrapper::onLocalCandidate(LocalCandidateCallback callback) {
    impl_->set_candidate_callback(std::move(callback));
}

void IceConnectivityWrapper::onLocalDescription(LocalDescriptionCallback callback) {
    impl_->set_description_callback(std::move(callback));
}

void IceConnectivityWrapper::onDataChannelOpen(DataChannelOpenCallback callback) {
    impl_->set_data_channel_open_callback(std::move(callback));
}

void IceConnectivityWrapper::onDataChannelClosed(DataChannelClosedCallback callback) {
    impl_->set_data_channel_closed_callback(std::move(callback));
}

void IceConnectivityWrapper::onDataChannelMessage(DataChannelMessageCallback callback) {
    impl_->set_data_channel_message_callback(std::move(callback));
}

void IceConnectivityWrapper::onDataChannelBinaryMessage(DataChannelBinaryMessageCallback callback) {
    impl_->set_data_channel_binary_message_callback(std::move(callback));
}

void IceConnectivityWrapper::onDataChannelWritable(DataChannelWritableCallback callback) {
    impl_->set_data_channel_writable_callback(std::move(callback));
}

bool IceConnectivityWrapper::sendDataChannelMessage(
    DataChannelKind kind,
    const std::string& message,
    std::string* error_detail) {
    return impl_->send_data_channel_message(kind, message, error_detail);
}

bool IceConnectivityWrapper::sendDataChannelBinaryMessage(
    DataChannelKind kind,
    std::span<const std::uint8_t> message,
    std::string* error_detail,
    DataChannelSendOutcome* outcome) {
    return impl_->send_data_channel_binary_message(kind, message, error_detail, outcome);
}

bool IceConnectivityWrapper::setDataChannelBufferedAmountLowThreshold(
    DataChannelKind kind,
    std::size_t threshold_bytes,
    std::string* error_detail) {
    return impl_->set_data_channel_buffered_amount_low_threshold(
        kind,
        threshold_bytes,
        error_detail);
}

bool IceConnectivityWrapper::getDataChannelTransportStats(
    DataChannelKind kind,
    DataChannelTransportStats* stats,
    std::string* error_detail) const {
    return impl_->get_data_channel_transport_stats(kind, stats, error_detail);
}

bool IceConnectivityWrapper::closeDataChannel(
    DataChannelKind kind,
    std::string* error_detail) {
    return impl_->close_data_channel(kind, error_detail);
}

bool IceConnectivityWrapper::ensureDataChannel(
    DataChannelKind kind,
    std::string* error_detail) {
    return impl_->ensure_data_channel(kind, error_detail);
}

IceConnectionState IceConnectivityWrapper::connection_state() const {
    return impl_->state();
}

void IceConnectivityWrapper::close() {
    impl_->close();
}


std::string_view module_name() {
    return "net";
}
}
