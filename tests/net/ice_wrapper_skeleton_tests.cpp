#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#endif

#include "redclaw/net/net_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_start_gathering_emits_state_callback() {
    redclaw::net::IceConnectivityWrapper wrapper;

    bool saw_gathering_state = false;
    wrapper.onConnectionStateChanged([&](redclaw::net::IceConnectionState state) {
        if (state == redclaw::net::IceConnectionState::kGathering) {
            saw_gathering_state = true;
        }
    });

    std::string error;
    const bool started = wrapper.startGathering({}, &error);

    return expect_true(started, std::string("startGathering should succeed: ") + error)
        && expect_true(saw_gathering_state, "state callback should observe gathering state");
}

bool test_fixed_udp_port_rejects_existing_exclusive_binding() {
#ifndef _WIN32
    return true;
#else
    WSADATA winsock_data{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
        return expect_true(false, "WSAStartup should succeed for fixed-port test");
    }
    const SOCKET socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) {
        WSACleanup();
        return expect_true(false, "test UDP socket should be created");
    }
    const BOOL exclusive = TRUE;
    if (setsockopt(
            socket_handle,
            SOL_SOCKET,
            SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive),
            sizeof(exclusive)) == SOCKET_ERROR) {
        closesocket(socket_handle);
        WSACleanup();
        return expect_true(false, "test UDP socket should enable exclusive binding");
    }
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = 0;
    endpoint.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_handle, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR) {
        closesocket(socket_handle);
        WSACleanup();
        return expect_true(false, "test UDP socket should bind an ephemeral port");
    }
    int endpoint_size = sizeof(endpoint);
    if (getsockname(
            socket_handle,
            reinterpret_cast<sockaddr*>(&endpoint),
            &endpoint_size) == SOCKET_ERROR) {
        closesocket(socket_handle);
        WSACleanup();
        return expect_true(false, "test UDP socket should expose its assigned port");
    }

    const auto port = ntohs(endpoint.sin_port);
    redclaw::net::IceConnectivityWrapper wrapper;
    std::string error;
    const bool reserved = wrapper.reserveFixedUdpPort(port, {}, &error);

    closesocket(socket_handle);
    WSACleanup();
    return expect_true(!reserved, "an occupied fixed ICE UDP port must reject runtime reservation")
        && expect_true(
            error.find("ICE UDP port " + std::to_string(port) + " is unavailable")
                != std::string::npos,
            "fixed-port failure should report the occupied port")
        && expect_true(
            error.find("native_error=") != std::string::npos,
            "fixed-port failure should report the native socket error");
#endif
}

bool test_fixed_udp_port_reservation_is_held_before_gathering() {
#ifndef _WIN32
    return true;
#else
    WSADATA winsock_data{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
        return expect_true(false, "WSAStartup should succeed for reservation lifetime test");
    }
    SOCKET allocator = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = 0;
    endpoint.sin_addr.s_addr = htonl(INADDR_ANY);
    if (allocator == INVALID_SOCKET
        || bind(allocator, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR) {
        if (allocator != INVALID_SOCKET) closesocket(allocator);
        WSACleanup();
        return expect_true(false, "reservation lifetime test should allocate a UDP port");
    }
    int endpoint_size = sizeof(endpoint);
    if (getsockname(allocator, reinterpret_cast<sockaddr*>(&endpoint), &endpoint_size) == SOCKET_ERROR) {
        closesocket(allocator);
        WSACleanup();
        return expect_true(false, "reservation lifetime test should read the UDP port");
    }
    const auto port = ntohs(endpoint.sin_port);
    closesocket(allocator);

    redclaw::net::IceConnectivityWrapper wrapper;
    std::string error;
    const bool reserved = wrapper.reserveFixedUdpPort(port, {}, &error);

    SOCKET competitor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const BOOL exclusive = TRUE;
    const bool competitor_configured = competitor != INVALID_SOCKET
        && setsockopt(
            competitor,
            SOL_SOCKET,
            SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive),
            sizeof(exclusive)) != SOCKET_ERROR;
    endpoint.sin_port = htons(port);
    const bool competitor_blocked = competitor_configured
        && bind(competitor, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR;
    if (competitor != INVALID_SOCKET) closesocket(competitor);
    (void)wrapper.shutdown();

    SOCKET after_shutdown = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const bool after_shutdown_configured = after_shutdown != INVALID_SOCKET
        && setsockopt(
            after_shutdown,
            SOL_SOCKET,
            SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive),
            sizeof(exclusive)) != SOCKET_ERROR;
    const bool released = after_shutdown_configured
        && bind(after_shutdown, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != SOCKET_ERROR;
    if (after_shutdown != INVALID_SOCKET) closesocket(after_shutdown);
    WSACleanup();

    return expect_true(reserved, std::string("fixed UDP port should be reserved: ") + error)
        && expect_true(competitor_blocked, "runtime reservation must exclude a competing bind")
        && expect_true(released, "runtime shutdown must release the reserved UDP port");
#endif
}

bool test_apply_remote_candidate_requires_initialized_peer() {
    redclaw::net::IceConnectivityWrapper wrapper;

    std::string error;
    const bool ok = wrapper.applyRemoteCandidate(
        "candidate:1 1 udp 2122260223 192.168.0.1 50000 typ host",
        "0",
        &error);

    return expect_true(!ok, "applyRemoteCandidate should fail before startGathering")
        && expect_true(wrapper.tryApplyRemoteCandidate(
            "candidate:1 1 udp 2122260223 192.168.0.1 50000 typ host", "0")
            == redclaw::net::RemoteCandidateApplyOutcome::kNotReady, "uninitialized peer should be not ready");
}

bool test_apply_remote_candidate_rejects_invalid_payload() {
    redclaw::net::IceConnectivityWrapper wrapper;

    std::string error;
    if (!wrapper.startGathering({}, &error)) {
        return expect_true(false, std::string("startGathering should succeed for invalid candidate test: ") + error);
    }

    error.clear();
    const bool ok = wrapper.applyRemoteCandidate("", "0", &error);

    return expect_true(!ok, "applyRemoteCandidate should reject empty candidate payload")
        && expect_true(!error.empty(), "validation failure should have a safe diagnostic");
}

bool test_apply_remote_candidate_rejects_invalid_candidate_prefix() {
    redclaw::net::IceConnectivityWrapper wrapper;

    std::string error;
    if (!wrapper.startGathering({}, &error)) {
        return expect_true(false, std::string("startGathering should succeed for invalid prefix test: ") + error);
    }

    error.clear();
    const bool ok = wrapper.applyRemoteCandidate(
        "not-a-candidate 1 udp 2122260223 192.168.0.1 50000 typ host",
        "0",
        &error);

    return expect_true(!ok, "applyRemoteCandidate should reject malformed candidate prefix")
        && expect_true(!error.empty(), "candidate format failure should have a safe diagnostic");
}

bool test_apply_remote_candidate_rejects_missing_typ_segment() {
    redclaw::net::IceConnectivityWrapper wrapper;

    std::string error;
    if (!wrapper.startGathering({}, &error)) {
        return expect_true(false, std::string("startGathering should succeed for missing typ test: ") + error);
    }

    error.clear();
    const bool ok = wrapper.applyRemoteCandidate(
        "candidate:1 1 udp 2122260223 192.168.0.1 50000 host",
        "0",
        &error);

    return expect_true(!ok, "applyRemoteCandidate should reject candidate without typ token")
        && expect_true(!error.empty(), "candidate format failure should have a safe diagnostic");
}

bool test_apply_remote_candidate_rejects_invalid_mid() {
    redclaw::net::IceConnectivityWrapper wrapper;

    std::string error;
    if (!wrapper.startGathering({}, &error)) {
        return expect_true(false, std::string("startGathering should succeed for invalid mid test: ") + error);
    }

    error.clear();
    const bool ok = wrapper.applyRemoteCandidate(
        "candidate:1 1 udp 2122260223 192.168.0.1 50000 typ host",
        "mid with space",
        &error);

    return expect_true(!ok, "applyRemoteCandidate should reject invalid mid token")
        && expect_true(!error.empty(), "mid format failure should have a safe diagnostic");
}

bool test_media_and_control_transport_state_are_independent() {
    redclaw::net::IceConnectivityWrapper wrapper;
    redclaw::net::DataChannelTransportStats media;
    redclaw::net::DataChannelTransportStats control;
    redclaw::net::DataChannelTransportStats agent;
    redclaw::net::DataChannelTransportStats navigation;
    redclaw::net::DataChannelTransportStats debug_bridge;
    std::string error;
    if (!wrapper.getDataChannelTransportStats(
            redclaw::net::DataChannelKind::kMedia, &media, &error)) {
        return expect_true(false, "media stats query should succeed before gathering: " + error);
    }
    if (!wrapper.getDataChannelTransportStats(
            redclaw::net::DataChannelKind::kControl, &control, &error)) {
        return expect_true(false, "control stats query should succeed before gathering: " + error);
    }
    if (!wrapper.getDataChannelTransportStats(
            redclaw::net::DataChannelKind::kAgent, &agent, &error)) {
        return expect_true(false, "agent stats query should succeed before gathering: " + error);
    }
    if (!wrapper.getDataChannelTransportStats(
            redclaw::net::DataChannelKind::kNavigation, &navigation, &error)) {
        return expect_true(false, "navigation stats query should succeed before gathering: " + error);
    }
    if (!wrapper.getDataChannelTransportStats(
            redclaw::net::DataChannelKind::kDebugBridge, &debug_bridge, &error)) {
        return expect_true(false, "debug bridge stats query should succeed before gathering: " + error);
    }
    return expect_true(
            !media.available && !control.available && !agent.available
                && !navigation.available
                && !debug_bridge.available,
                       "all typed channels should report their own unavailable state")
        && expect_true(
            redclaw::net::kMediaDataChannelLabel != redclaw::net::kControlDataChannelLabel,
            "media and control labels must stay distinct")
        && expect_true(
            redclaw::net::kAgentDataChannelLabel != redclaw::net::kControlDataChannelLabel,
            "agent and control labels must stay distinct")
        && expect_true(
            redclaw::net::kNavigationDataChannelLabel
                != redclaw::net::kAgentDataChannelLabel,
            "navigation and agent labels must stay distinct")
        && expect_true(
            redclaw::net::kDebugBridgeDataChannelLabel
                != redclaw::net::kAgentDataChannelLabel,
            "debug bridge and agent labels must stay distinct");
}

bool test_media_is_realtime_while_control_remains_reliable() {
    const auto media =
        redclaw::net::data_channel_delivery_policy(redclaw::net::DataChannelKind::kMedia);
    const auto control =
        redclaw::net::data_channel_delivery_policy(redclaw::net::DataChannelKind::kControl);
    const auto agent =
        redclaw::net::data_channel_delivery_policy(redclaw::net::DataChannelKind::kAgent);
    const auto navigation =
        redclaw::net::data_channel_delivery_policy(redclaw::net::DataChannelKind::kNavigation);
    const auto debug_bridge =
        redclaw::net::data_channel_delivery_policy(
            redclaw::net::DataChannelKind::kDebugBridge);
    const auto terminal =
        redclaw::net::data_channel_delivery_policy(redclaw::net::DataChannelKind::kTerminal);
    const auto transfer =
        redclaw::net::data_channel_delivery_policy(redclaw::net::DataChannelKind::kTransfer);
    return expect_true(transfer.reliable && transfer.ordered, "file transfer needs reliable ordered bulk delivery")
        && expect_true(!media.reliable, "obsolete media must not block the stream behind retransmissions")
        && expect_true(media.ordered, "media fragments should preserve order within delivered data")
        && expect_true(media.max_retransmits == 0, "media should use zero retransmits")
        && expect_true(control.reliable, "control messages must remain reliable")
        && expect_true(control.ordered, "control messages must remain ordered")
        && expect_true(agent.reliable, "agent messages must be reliable")
        && expect_true(agent.ordered, "agent messages must be ordered")
        && expect_true(!navigation.reliable, "navigation thumbnails must be latest-only")
        && expect_true(!navigation.ordered, "navigation thumbnails must not wait for stale packets")
        && expect_true(navigation.max_retransmits == 0, "navigation must use zero retransmits")
        && expect_true(
            redclaw::net::kNavigationDataChannelLabel == "redclaw-navigation-v1",
            "navigation channel label must be versioned")
        && expect_true(debug_bridge.reliable, "debug bridge messages must be reliable")
        && expect_true(debug_bridge.ordered, "debug bridge messages must be ordered")
        && expect_true(terminal.reliable && terminal.ordered, "terminal control bytes must not be dropped or reordered")
        && expect_true(
            redclaw::net::kDebugBridgeDataChannelLabel == "redclaw-debug-bridge-v1",
            "debug bridge channel label must be versioned");
}

bool test_only_optional_channels_can_be_rebuilt_independently() {
    redclaw::net::IceConnectivityWrapper wrapper;
    std::string error;
    const bool control = wrapper.ensureDataChannel(
        redclaw::net::DataChannelKind::kControl, &error);
    return expect_true(!control, "required channels must not use optional rebuild path")
        && expect_true(error.find("optional data channel") != std::string::npos,
                       "rebuild error should describe the optional-channel boundary");
}

bool test_deferred_answer_is_not_emitted_automatically() {
    using namespace std::chrono_literals;

    std::mutex mutex;
    std::condition_variable ready;
    std::string host_offer;
    std::vector<bool> controller_description_types;
    redclaw::net::IceConnectivityWrapper host;
    redclaw::net::IceConnectivityWrapper controller;

    host.onLocalDescription([&](const std::string& sdp, bool is_offer) {
        if (!is_offer) {
            return;
        }
        {
            std::lock_guard lock(mutex);
            host_offer = sdp;
        }
        ready.notify_all();
    });
    controller.onLocalDescription([&](const std::string&, bool is_offer) {
        {
            std::lock_guard lock(mutex);
            controller_description_types.push_back(is_offer);
        }
        ready.notify_all();
    });

    redclaw::net::IceGatheringConfig controller_config;
    controller_config.initiate_offer = false;
    std::string error;
    if (!controller.startGathering(controller_config, &error)) {
        return expect_true(false, "controller deferred-answer gathering should start: " + error);
    }

    redclaw::net::IceGatheringConfig host_config;
    host_config.initiate_offer = true;
    host_config.data_channels = {redclaw::net::DataChannelKind::kDebugBridge};
    if (!host.startGathering(host_config, &error)) {
        return expect_true(false, "host deferred-answer gathering should start: " + error);
    }

    {
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, 5s, [&]() { return !host_offer.empty(); })) {
            return expect_true(false, "host should emit an offer for deferred-answer test");
        }
    }

    std::string offer;
    {
        std::lock_guard lock(mutex);
        offer = host_offer;
    }
    if (!controller.applyRemoteDescription(offer, true, &error, false)) {
        return expect_true(false, "controller should apply offer without answering: " + error);
    }

    bool answered_early = false;
    {
        std::unique_lock lock(mutex);
        answered_early = ready.wait_for(lock, 250ms, [&]() {
            return !controller_description_types.empty();
        });
    }
    if (answered_early) {
        host.close();
        controller.close();
        return expect_true(false, "auto_answer=false must suppress the local answer");
    }

    if (!controller.emitLocalDescription(&error)) {
        return expect_true(false, "controller should explicitly emit deferred answer: " + error);
    }
    {
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, 5s, [&]() {
                return !controller_description_types.empty();
            })) {
            return expect_true(false, "controller should emit the deferred answer");
        }
    }

    std::vector<bool> description_types;
    {
        std::lock_guard lock(mutex);
        description_types = controller_description_types;
    }
    host.close();
    controller.close();
    return expect_true(
               description_types.size() == 1,
               "deferred answer path must emit exactly one local description")
        && expect_true(
            !description_types.front(),
            "deferred local description must be an answer");
}

bool test_first_optional_channel_explicitly_starts_offer() {
    using namespace std::chrono_literals;

    std::mutex mutex;
    std::condition_variable ready;
    std::vector<bool> description_types;
    redclaw::net::IceConnectivityWrapper host;
    host.onLocalDescription([&](const std::string&, bool is_offer) {
        {
            std::lock_guard lock(mutex);
            description_types.push_back(is_offer);
        }
        ready.notify_all();
    });

    redclaw::net::IceGatheringConfig config;
    config.initiate_offer = true;
    config.data_channels.clear();
    std::string error;
    if (!host.startGathering(config, &error)) {
        return expect_true(false, "empty host gathering should start: " + error);
    }
    {
        std::unique_lock lock(mutex);
        if (ready.wait_for(lock, 100ms, [&]() { return !description_types.empty(); })) {
            return expect_true(false, "host without channels must not emit an empty offer");
        }
    }
    if (!expect_true(
            host.connection_state() == redclaw::net::IceConnectionState::kGathering,
            "host without channels must stay initialized while offer emission is deferred")) {
        host.close();
        return false;
    }

    if (!host.ensureDataChannel(redclaw::net::DataChannelKind::kAgent, &error)) {
        return expect_true(false, "first optional channel should be created: " + error);
    }
    {
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, 5s, [&]() { return !description_types.empty(); })) {
            return expect_true(false, "first optional channel should explicitly emit an offer");
        }
    }

    std::vector<bool> observed;
    {
        std::lock_guard lock(mutex);
        observed = description_types;
    }
    host.close();
    return expect_true(observed.size() == 1, "first optional channel should emit one description")
        && expect_true(observed.front(), "first optional channel must emit an offer");
}

bool test_closing_unavailable_typed_channel_fails_closed() {
    redclaw::net::IceConnectivityWrapper wrapper;
    std::string error;

    const bool closed = wrapper.closeDataChannel(
        redclaw::net::DataChannelKind::kControl, &error);

    return expect_true(!closed, "an unavailable typed channel must not report a successful close")
        && expect_true(
            error.find("not available") != std::string::npos,
            "the close failure should identify the missing typed channel");
}

bool test_preferred_stun_server_keeps_responsive_primary() {
    const std::vector<std::string> configured{
        "stun:primary.example.com:3478",
        "stun:secondary.example.com:3478",
    };
    const std::vector<redclaw::net::StunServerProbeResult> probes{
        {configured[0], true, 80},
        {configured[1], true, 20},
    };
    return expect_true(
        redclaw::net::choose_preferred_stun_server(configured, probes) == configured[0],
        "a responsive primary STUN server should retain configured priority");
}

bool test_preferred_stun_server_uses_fastest_responsive_alternative() {
    const std::vector<std::string> configured{
        "stun:primary.example.com:3478",
        "stun:secondary.example.com:3478",
        "stun:tertiary.example.com:3478",
    };
    const std::vector<redclaw::net::StunServerProbeResult> probes{
        {configured[0], false, 0},
        {configured[1], true, 80},
        {configured[2], true, 20},
    };
    return expect_true(
        redclaw::net::choose_preferred_stun_server(configured, probes) == configured[2],
        "the fastest responsive alternative should be selected when the primary is unavailable");
}

bool test_preferred_stun_server_falls_back_to_first_configured() {
    const std::vector<std::string> configured{
        "stun:primary.example.com:3478",
        "stun:secondary.example.com:3478",
    };
    const std::vector<redclaw::net::StunServerProbeResult> probes{
        {configured[0], false, 0},
        {configured[1], false, 0},
    };
    return expect_true(
        redclaw::net::choose_preferred_stun_server(configured, probes) == configured[0],
        "failed probes should preserve deterministic configured-order fallback");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_start_gathering_emits_state_callback() && ok;
    ok = test_fixed_udp_port_rejects_existing_exclusive_binding() && ok;
    ok = test_fixed_udp_port_reservation_is_held_before_gathering() && ok;
    ok = test_apply_remote_candidate_requires_initialized_peer() && ok;
    ok = test_apply_remote_candidate_rejects_invalid_payload() && ok;
    ok = test_apply_remote_candidate_rejects_invalid_candidate_prefix() && ok;
    ok = test_apply_remote_candidate_rejects_missing_typ_segment() && ok;
    ok = test_apply_remote_candidate_rejects_invalid_mid() && ok;
    ok = test_media_and_control_transport_state_are_independent() && ok;
    ok = test_closing_unavailable_typed_channel_fails_closed() && ok;
    ok = test_media_is_realtime_while_control_remains_reliable() && ok;
    ok = test_only_optional_channels_can_be_rebuilt_independently() && ok;
    ok = test_deferred_answer_is_not_emitted_automatically() && ok;
    ok = test_first_optional_channel_explicitly_starts_offer() && ok;
    ok = test_preferred_stun_server_keeps_responsive_primary() && ok;
    ok = test_preferred_stun_server_uses_fastest_responsive_alternative() && ok;
    ok = test_preferred_stun_server_falls_back_to_first_configured() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_net_ice_wrapper_skeleton_tests" << '\n';
    return 0;
}
