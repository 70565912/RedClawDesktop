#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "redclaw/net/net_module.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
using namespace redclaw::net;
using namespace std::chrono_literals;
#ifdef _WIN32
// Two advertised loopback mappings. A peer may receive only after it has sent
// through its mapping. No system routing/firewall changes; no packet queue.
class FilteredMappings {
public:
    explicit FilteredMappings(bool mismatch, bool malformed = false) : mismatch_(mismatch), malformed_(malformed) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSA startup failed");
        try {
            for (std::size_t i = 0; i < sockets_.size(); ++i) {
                sockets_[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (sockets_[i] == INVALID_SOCKET) throw std::runtime_error("socket failed");
                auto address = loopback(0);
                if (bind(sockets_[i], reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
                    throw std::runtime_error("loopback bind failed");
                int length = sizeof(address);
                if (getsockname(sockets_[i], reinterpret_cast<sockaddr*>(&address), &length) != 0)
                    throw std::runtime_error("socket address failed");
                ports_[i] = ntohs(address.sin_port);
            }
            worker_ = std::jthread([this](std::stop_token token) { forward(token); });
        } catch (...) { cleanup(); throw; }
    }
    ~FilteredMappings() { worker_.request_stop(); if (worker_.joinable()) worker_.join(); cleanup(); }
    unsigned short external_port(std::size_t owner) const { return ports_[owner]; }
    std::string translate(std::size_t owner, const std::string& candidate) {
        std::istringstream input(candidate);
        std::vector<std::string> fields;
        for (std::string field; input >> field;) fields.push_back(field);
        if (fields.size() < 8 || fields[2] != "UDP") {
            // libjuice uses lowercase udp; other transports are deliberately unsupported here.
            if (fields.size() < 8 || fields[2] != "udp") return {};
        }
        if (fields[4] != "127.0.0.1" || fields[7] != "host") return {};
        const auto port = std::stoul(fields[5]);
        if (port == 0 || port > 65535) return {};
        actual_ports_[owner].store(static_cast<unsigned short>(port));
        fields[5] = std::to_string(ports_[owner]);
        std::string result;
        for (const auto& field : fields) { if (!result.empty()) result += ' '; result += field; }
        return result;
    }
    std::atomic<unsigned> filtered{0}, forwarded{0}, peer_unreachable{0}, errors{0};
    bool inject(std::size_t owner, const std::vector<char>& bytes) {
        auto target = loopback(actual_ports_[owner].load());
        return sendto(sockets_[1 - owner], bytes.data(), static_cast<int>(bytes.size()), 0,
            reinterpret_cast<sockaddr*>(&target), sizeof(target)) == static_cast<int>(bytes.size());
    }
private:
    static sockaddr_in loopback(unsigned short port) {
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
        return address;
    }
    void cleanup() {
        for (auto& socket_handle : sockets_) if (socket_handle != INVALID_SOCKET) {
            closesocket(socket_handle); socket_handle = INVALID_SOCKET;
        }
        WSACleanup();
    }
    void forward(std::stop_token token) {
        std::array<char, 65536> packet{};
        std::array<bool, 2> sent{};
        while (!token.stop_requested()) {
            fd_set ready; FD_ZERO(&ready);
            for (auto socket_handle : sockets_) FD_SET(socket_handle, &ready);
            timeval timeout{0, 10000};
            const int count = select(0, &ready, nullptr, nullptr, &timeout);
            if (count < 0) { ++errors; return; }
            for (std::size_t i = 0; i < sockets_.size(); ++i) {
                if (!FD_ISSET(sockets_[i], &ready)) continue;
                sockaddr_in source{}; int length = sizeof(source);
                const int size = recvfrom(sockets_[i], packet.data(), static_cast<int>(packet.size()), 0,
                    reinterpret_cast<sockaddr*>(&source), &length);
                // Windows reports an earlier ICMP port-unreachable on recvfrom.
                // Expected when a native peer has already failed/closed, not a proxy defect.
                if (size < 0 && WSAGetLastError() == WSAECONNRESET) { ++peer_unreachable; continue; }
                if (size <= 0) { ++errors; continue; }
                if (source.sin_addr.s_addr != htonl(INADDR_LOOPBACK)
                    || ntohs(source.sin_port) != actual_ports_[1 - i].load()) { ++errors; continue; }
                sent[i] = true;
                // mismatch models an advertised mapping whose inbound permission never matches.
                if (!sent[1 - i] || mismatch_ || actual_ports_[i].load() == 0) { ++filtered; continue; }
                auto destination = loopback(actual_ports_[i].load());
                if (malformed_ && size >= 20) packet[static_cast<std::size_t>(size - 1)] ^= 1; // invalid fingerprint
                if (sendto(sockets_[1 - i], packet.data(), size, 0,
                    reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) != size) ++errors;
                else ++forwarded;
            }
        }
    }
    bool mismatch_;
    bool malformed_;
    std::array<SOCKET, 2> sockets_{INVALID_SOCKET, INVALID_SOCKET};
    std::array<unsigned short, 2> ports_{};
    std::array<std::atomic<unsigned short>, 2> actual_ports_{};
    std::jthread worker_;
};

class LocalDiscoveryServer {
public:
    LocalDiscoveryServer() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("STUN fixture WSA failed");
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof(address);
        if (socket_ == INVALID_SOCKET || bind(socket_, reinterpret_cast<sockaddr*>(&address), length) != 0
            || getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            if (socket_ != INVALID_SOCKET) closesocket(socket_);
            WSACleanup(); throw std::runtime_error("STUN fixture bind failed");
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::jthread([this](std::stop_token token) {
            std::array<char, 2048> request{};
            while (!token.stop_requested()) {
                fd_set ready; FD_ZERO(&ready); FD_SET(socket_, &ready); timeval timeout{0, 10000};
                if (select(0, &ready, nullptr, nullptr, &timeout) <= 0) continue;
                sockaddr_in source{}; int size = sizeof(source);
                const int bytes = recvfrom(socket_, request.data(), static_cast<int>(request.size()), 0,
                    reinterpret_cast<sockaddr*>(&source), &size);
                if (bytes < 20 || request[0] != 0 || request[1] != 1) continue;
                std::array<char, 32> reply{};
                std::copy_n(request.data(), 20, reply.data());
                reply[0] = 1; reply[1] = 1; reply[2] = 0; reply[3] = 12;
                reply[20] = 0; reply[21] = 0x20; reply[22] = 0; reply[23] = 8;
                reply[25] = 1;
                const unsigned port = ntohs(source.sin_port) ^ 0x2112U;
                reply[26] = static_cast<char>(port >> 8); reply[27] = static_cast<char>(port);
                const auto ip = ntohl(source.sin_addr.s_addr) ^ 0x2112a442U;
                for (unsigned i = 0; i < 4; ++i) reply[28 + i] = static_cast<char>(ip >> (24 - i * 8));
                (void)sendto(socket_, reply.data(), static_cast<int>(reply.size()), 0,
                    reinterpret_cast<sockaddr*>(&source), sizeof(source));
            }
        });
    }
    ~LocalDiscoveryServer() { worker_.request_stop(); worker_.join(); closesocket(socket_); WSACleanup(); }
    std::string uri() const { return "stun:127.0.0.1:" + std::to_string(port_); }
private:
    SOCKET socket_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::jthread worker_;
};

std::string description_without_candidates(const std::string& sdp, bool wrong_password) {
    std::istringstream input(sdp); std::string result;
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("a=candidate:") || line == "a=end-of-candidates") continue;
        if (wrong_password && line.starts_with("a=ice-pwd:") && line.size() > 10)
            line[10] = line[10] == 'a' ? 'b' : 'a';
        result += line + "\r\n";
    }
    return result;
}

struct Scenario {
    int answer_delay_s;
    bool mapping_mismatch;
    bool wrong_password;
    const char* name;
    std::uint32_t initial_check_window_ms = 0;
    bool malformed_stun = false;
};
void PrintTo(const Scenario& scenario, std::ostream* out) { *out << scenario.name; }
class FilteredIce : public ::testing::TestWithParam<Scenario> {};
TEST(FilteredIce, RouterAssignedPortIsPublishedAndConnectsToUnchangedPeer) {
    FilteredMappings mappings(false);
    // Reserve an available base port; the proxy's externally advertised port is different.
    const SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_NE(probe, INVALID_SOCKET);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(address);
    const bool bound = bind(probe, reinterpret_cast<sockaddr*>(&address), length) == 0
        && getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    closesocket(probe);
    ASSERT_TRUE(bound);
    const auto internal_port = ntohs(address.sin_port);
    ASSERT_NE(internal_port, mappings.external_port(0));

    std::mutex mutex; std::condition_variable cv;
    std::string offer, answer, mapped_candidate, peer_candidate, host_mid, peer_mid, received;
    unsigned opened = 0;
    IceConnectivityWrapper host, controller;
    host.onLocalDescription([&](const auto& sdp, bool is_offer) {
        if (is_offer) { std::lock_guard lock(mutex); offer = description_without_candidates(sdp, false); cv.notify_all(); }
    });
    controller.onLocalDescription([&](const auto& sdp, bool is_offer) {
        if (!is_offer) { std::lock_guard lock(mutex); answer = description_without_candidates(sdp, false); cv.notify_all(); }
    });
    host.onLocalCandidate([&](const auto& candidate, const auto& mid) {
        (void)mappings.translate(0, candidate); // fixture learns the native socket, not the advertised mapping.
        if (candidate.find(" typ srflx ") != std::string::npos) {
            std::lock_guard lock(mutex); mapped_candidate = candidate; host_mid = mid; cv.notify_all();
        }
    });
    controller.onLocalCandidate([&](const auto& candidate, const auto& mid) {
        auto translated = mappings.translate(1, candidate);
        if (!translated.empty()) { std::lock_guard lock(mutex); peer_candidate = std::move(translated); peer_mid = mid; cv.notify_all(); }
    });
    auto on_open = [&](DataChannelKind kind) {
        if (kind == DataChannelKind::kControl) { std::lock_guard lock(mutex); ++opened; cv.notify_all(); }
    };
    host.onDataChannelOpen(on_open); controller.onDataChannelOpen(on_open);
    controller.onDataChannelMessage([&](DataChannelKind kind, const std::string& message) {
        if (kind == DataChannelKind::kControl) { std::lock_guard lock(mutex); received = message; cv.notify_all(); }
    });
    IceGatheringConfig config; config.bind_address = "127.0.0.1";
    config.data_channels = {DataChannelKind::kControl}; config.initiate_offer = false;
    ASSERT_TRUE(controller.startGathering(config)); // existing peer has no new mapping configuration.
    config.initiate_offer = true;
    config.port_range_begin = config.port_range_end = internal_port;
    config.udp_port_mapping = IceUdpPortMapping{"127.0.0.1", internal_port, "127.0.0.1", mappings.external_port(0)};
    ASSERT_TRUE(host.startGathering(config));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return !offer.empty() && !mapped_candidate.empty(); }));
        EXPECT_NE(mapped_candidate.find("127.0.0.1 " + std::to_string(mappings.external_port(0)) + " typ srflx"), std::string::npos);
        EXPECT_NE(mapped_candidate.find("rport " + std::to_string(internal_port)), std::string::npos);
    }
    ASSERT_TRUE(controller.applyRemoteDescription(offer, true));
    ASSERT_TRUE(controller.applyRemoteCandidate(mapped_candidate, host_mid)); // no direct host route reaches the peer.
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return !answer.empty() && !peer_candidate.empty(); })); }
    ASSERT_TRUE(host.applyRemoteDescription(answer, false));
    ASSERT_TRUE(host.applyRemoteCandidate(peer_candidate, peer_mid));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 18s, [&] { return opened == 2; })); }
    ASSERT_TRUE(host.sendDataChannelMessage(DataChannelKind::kControl, "mapped-port-roundtrip"));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return received == "mapped-port-roundtrip"; })); }
    ASSERT_TRUE(host.shutdown()); ASSERT_TRUE(controller.shutdown());
    EXPECT_GT(mappings.forwarded.load(), 0U);
    EXPECT_EQ(mappings.errors.load(), 0U);
    EXPECT_GT(host.diagnostics().ice_check_totals[static_cast<std::size_t>(IceCheckKind::kResponseRx)], 0U);
}

TEST_P(FilteredIce, ProductionChecksDistinguishFilteringFromCredentialRejection) {
    const auto scenario = GetParam();
    FilteredMappings mappings(scenario.mapping_mismatch, scenario.malformed_stun);
    std::mutex mutex; std::condition_variable cv;
    std::string offer, answer, host_candidate, controller_candidate;
    std::string host_mid, controller_mid;
    unsigned opened = 0; bool failed = false;
    IceConnectivityWrapper controller, host;
    host.onLocalDescription([&](const auto& sdp, bool is_offer) {
        if (is_offer) { std::lock_guard lock(mutex); if (offer.empty()) offer = description_without_candidates(sdp, scenario.wrong_password); cv.notify_all(); }
    });
    controller.onLocalDescription([&](const auto& sdp, bool is_offer) {
        if (!is_offer) { std::lock_guard lock(mutex); if (answer.empty()) answer = description_without_candidates(sdp, false); cv.notify_all(); }
    });
    host.onLocalCandidate([&](const auto& candidate, const auto& mid) {
        auto translated = mappings.translate(0, candidate);
        if (!translated.empty()) { std::lock_guard lock(mutex); if (host_candidate.empty()) { host_candidate = std::move(translated); host_mid = mid; } cv.notify_all(); }
    });
    controller.onLocalCandidate([&](const auto& candidate, const auto& mid) {
        auto translated = mappings.translate(1, candidate);
        if (!translated.empty()) { std::lock_guard lock(mutex); if (controller_candidate.empty()) { controller_candidate = std::move(translated); controller_mid = mid; } cv.notify_all(); }
    });
    auto opened_callback = [&](DataChannelKind kind) {
        if (kind == DataChannelKind::kMedia || kind == DataChannelKind::kControl) { std::lock_guard lock(mutex); ++opened; cv.notify_all(); }
    };
    controller.onDataChannelOpen(opened_callback); host.onDataChannelOpen(opened_callback);
    controller.onConnectionStateChanged([&](auto state) {
        if (state == IceConnectionState::kFailed) { std::lock_guard lock(mutex); failed = true; cv.notify_all(); }
    });
    IceGatheringConfig config; config.bind_address = "127.0.0.1"; config.initiate_offer = false;
    config.initial_check_window_ms = scenario.initial_check_window_ms;
    config.data_channels = {DataChannelKind::kMedia, DataChannelKind::kControl};
    ASSERT_TRUE(controller.startGathering(config));
    config.initiate_offer = true; ASSERT_TRUE(host.startGathering(config));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return !offer.empty() && !host_candidate.empty(); }));
    }
    ASSERT_TRUE(controller.applyRemoteDescription(offer, true));
    ASSERT_TRUE(controller.applyRemoteCandidate(host_candidate, host_mid));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return !answer.empty() && !controller_candidate.empty(); }));
        cv.wait_for(lock, std::chrono::seconds(scenario.answer_delay_s), [&] { return failed; });
    }
    const bool expect_expiry = scenario.answer_delay_s >= 40 && scenario.initial_check_window_ms == 0;
    const auto answer_delivered_at = std::chrono::steady_clock::now();
    if (!expect_expiry) {
        ASSERT_TRUE(host.applyRemoteDescription(answer, false));
        ASSERT_TRUE(host.applyRemoteCandidate(controller_candidate, controller_mid));
        std::unique_lock lock(mutex);
        if (scenario.mapping_mismatch || scenario.wrong_password || scenario.malformed_stun) {
            cv.wait_for(lock, 3s, [&] { return opened != 0; }); EXPECT_EQ(opened, 0U);
        } else {
            EXPECT_TRUE(cv.wait_for(lock, 18s, [&] { return opened == 4; }));
            RecordProperty("answer_to_channels_ms", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - answer_delivered_at).count()));
        }
    } else {
        // Expiry before Answer delivery is the expected reproduced failure, not a repaired path.
        { std::lock_guard lock(mutex); EXPECT_TRUE(failed); EXPECT_EQ(opened, 0U); }
        ASSERT_TRUE(host.applyRemoteDescription(answer, false));
        ASSERT_TRUE(host.applyRemoteCandidate(controller_candidate, controller_mid));
        std::unique_lock lock(mutex);
        cv.wait_for(lock, 1s, [&] { return opened != 0; }); EXPECT_EQ(opened, 0U);
    }
    ASSERT_TRUE(controller.shutdown()); ASSERT_TRUE(host.shutdown());
    const auto c = controller.diagnostics(), h = host.diagnostics();
    const auto count = [](const auto& d, IceCheckKind kind) { return d.ice_check_totals[static_cast<std::size_t>(kind)]; };
    EXPECT_GT(count(c, IceCheckKind::kRequestTx), 0U);
    EXPECT_EQ(mappings.errors.load(), 0U);
    EXPECT_LE(c.recent_events.size(), 64U); EXPECT_LE(h.recent_events.size(), 64U);
    EXPECT_GT(count(c, IceCheckKind::kSocketReady), 0U);
    EXPECT_GT(count(c, IceCheckKind::kLocalCandidateSocketMatch), 0U);
    EXPECT_EQ(count(c, IceCheckKind::kLocalCandidateSocketMismatch), 0U);
    EXPECT_GT(count(c, IceCheckKind::kSendTargetMatch), 0U);
    EXPECT_EQ(count(c, IceCheckKind::kSendTargetMismatch), 0U);
    if (scenario.mapping_mismatch || expect_expiry) {
        EXPECT_EQ(count(c, IceCheckKind::kRequestRx), 0U);
        EXPECT_EQ(count(c, IceCheckKind::kResponseRx), 0U);
        EXPECT_EQ(count(h, IceCheckKind::kValidationFailed), 0U);
        EXPECT_GT(mappings.filtered.load(), 0U);
    }
    if (scenario.mapping_mismatch) EXPECT_EQ(count(c, IceCheckKind::kSocketRx), 0U);
    if (scenario.wrong_password) {
        EXPECT_GT(count(h, IceCheckKind::kSocketRx), 0U);
        EXPECT_GT(count(h, IceCheckKind::kPeerStunParsed), 0U);
        EXPECT_GT(count(h, IceCheckKind::kValidationFailed), 0U);
        EXPECT_EQ(count(h, IceCheckKind::kStunParseFailed), 0U);
    }
    if (scenario.malformed_stun) {
        EXPECT_GT(count(h, IceCheckKind::kSocketRx), 0U);
        EXPECT_GT(count(h, IceCheckKind::kStunParseFailed), 0U);
        EXPECT_EQ(count(h, IceCheckKind::kValidationFailed), 0U);
        EXPECT_EQ(count(h, IceCheckKind::kRequestRx), 0U);
    }
    if (expect_expiry) EXPECT_GT(count(c, IceCheckKind::kTimeout), 0U);
    if (scenario.initial_check_window_ms != 0 && !scenario.mapping_mismatch && !scenario.wrong_password && !scenario.malformed_stun) {
        EXPECT_EQ(count(c, IceCheckKind::kTimeout), 0U);
        // Native burst plus one packet per 2 s, no flood while DHT is pending.
        EXPECT_LE(count(c, IceCheckKind::kRequestTx), 70U);
    }
    RecordProperty("filtered_packets", mappings.filtered.load());
    RecordProperty("forwarded_packets", mappings.forwarded.load());
    RecordProperty("peer_unreachable", mappings.peer_unreachable.load());
    RecordProperty("controller_request_tx", std::to_string(count(c, IceCheckKind::kRequestTx)));
    RecordProperty("host_validation_failed", std::to_string(count(h, IceCheckKind::kValidationFailed)));
    RecordProperty("required_channel_open_events", opened);
    RecordProperty("initial_check_window_ms", scenario.initial_check_window_ms);
    const auto generation = c.ice_check_generation;
    EXPECT_FALSE(controller.startGathering(config)); // shutdown is final and rejects new callbacks.
    EXPECT_EQ(controller.diagnostics().ice_check_generation, generation);
}
INSTANTIATE_TEST_SUITE_P(DelayedAnswerAndFiltering, FilteredIce, ::testing::Values(
    Scenario{0, false, false, "Ready"}, Scenario{22, false, false, "Answer22Seconds"},
    Scenario{33, false, false, "Answer33Seconds"}, Scenario{45, false, false, "AnswerAfterIceExpiry"},
    Scenario{0, true, false, "MappingMismatch"}, Scenario{0, false, true, "CredentialMismatch"},
    Scenario{0, false, false, "DhtReady", 120000},
    Scenario{45, false, false, "DhtAnswer45Seconds", 120000},
    Scenario{90, false, false, "DhtAnswer90Seconds", 120000},
    Scenario{0, false, true, "DhtCredentialMismatch", 120000},
    Scenario{0, false, false, "MalformedStun", 120000, true}),
    [](const auto& info) { return info.param.name; });

TEST(FilteredIce, InitialWindowCannotBeRenewedByTrickleAndCloseDrains) {
    FilteredMappings mappings(true);
    std::mutex mutex; std::condition_variable cv;
    std::string offer, candidate, mid;
    bool failed = false;
    unsigned opened = 0;
    IceConnectivityWrapper controller, host;
    host.onLocalDescription([&](const auto& sdp, bool is_offer) {
        if (is_offer) { std::lock_guard lock(mutex); offer = description_without_candidates(sdp, false); cv.notify_all(); }
    });
    host.onLocalCandidate([&](const auto& sdp, const auto& candidate_mid) {
        auto translated = mappings.translate(0, sdp);
        if (!translated.empty()) { std::lock_guard lock(mutex); candidate = translated; mid = candidate_mid; cv.notify_all(); }
    });
    controller.onLocalCandidate([&](const auto& sdp, const auto&) { (void)mappings.translate(1, sdp); });
    controller.onConnectionStateChanged([&](auto state) {
        if (state == IceConnectionState::kFailed) { std::lock_guard lock(mutex); failed = true; cv.notify_all(); }
    });
    controller.onDataChannelOpen([&](auto) { std::lock_guard lock(mutex); ++opened; });
    IceGatheringConfig config; config.bind_address = "127.0.0.1";
    config.initial_check_window_ms = 120001;
    EXPECT_FALSE(controller.startGathering(config));
    // Above native RFC 8863 PAC (39.5 s): that minimum is deliberately not bypassed.
    config.initial_check_window_ms = 45000; config.initiate_offer = false;
    ASSERT_TRUE(controller.startGathering(config));
    config.initiate_offer = true; ASSERT_TRUE(host.startGathering(config));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return !offer.empty() && !candidate.empty(); })); }
    const auto started = std::chrono::steady_clock::now();
    ASSERT_TRUE(controller.applyRemoteDescription(offer, true));
    ASSERT_TRUE(controller.applyRemoteCandidate(candidate, mid));
    // Repeated/late trickle cannot renew the initial window or bring a failed peer back.
    for (unsigned i = 0; i != 5; ++i) {
        { std::unique_lock lock(mutex); cv.wait_for(lock, 8s, [&] { return failed; }); }
        ASSERT_TRUE(controller.applyRemoteCandidate(candidate, mid));
    }
    { std::unique_lock lock(mutex);
      EXPECT_TRUE(cv.wait_for(lock, 8s, [&] { return failed; })); EXPECT_EQ(opened, 0U); }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_GE(elapsed, 44s); EXPECT_LT(elapsed, 48s);
    const auto totals = controller.diagnostics().ice_check_totals;
    EXPECT_GT(totals[static_cast<std::size_t>(IceCheckKind::kTimeout)], 0U);
    EXPECT_LE(totals[static_cast<std::size_t>(IceCheckKind::kRequestTx)], 20U);
    EXPECT_EQ(totals[static_cast<std::size_t>(IceCheckKind::kRequestRx)], 0U);
    const auto closing = std::chrono::steady_clock::now();
    ASSERT_TRUE(controller.shutdown()); ASSERT_TRUE(host.shutdown());
    EXPECT_LT(std::chrono::steady_clock::now() - closing, 2s);
    EXPECT_EQ(mappings.errors.load(), 0U);
    RecordProperty("elapsed_ms", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
}

TEST(FilteredIce, PreDispatchFloodIsCountedWithoutUnboundedLogsAndResetsWithPeer) {
    FilteredMappings mappings(false);
    std::mutex mutex;
    std::condition_variable cv;
    unsigned candidates = 0;
    IceConnectivityWrapper host;
    host.onLocalCandidate([&](const auto& candidate, const auto&) {
        if (!mappings.translate(0, candidate).empty()) {
            std::lock_guard lock(mutex); ++candidates; cv.notify_all();
        }
    });
    IceGatheringConfig config; config.bind_address = "127.0.0.1";
    config.initiate_offer = true;
    ASSERT_TRUE(host.startGathering(config));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return candidates != 0; })); }
    const auto before = host.diagnostics();
    // Valid uncredentialed Binding request from an unregistered address: parsed,
    // then dropped by address dispatch, distinct from missing/invalid integrity.
    std::vector<char> unknown_binding(20, 0);
    unknown_binding[1] = 1;
    unknown_binding[4] = 0x21; unknown_binding[5] = 0x12;
    unknown_binding[6] = static_cast<char>(0xa4); unknown_binding[7] = 0x42;
    ASSERT_TRUE(mappings.inject(0, unknown_binding));
    for (unsigned i = 0; i < 1024; ++i)
        ASSERT_TRUE(mappings.inject(0, std::vector<char>(24, static_cast<char>(0xff))));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    const auto count = [](const auto& d, IceCheckKind k) { return d.ice_check_totals[static_cast<std::size_t>(k)]; };
    auto observed = host.diagnostics();
    while ((count(observed, IceCheckKind::kNonStunPreselection) < 100
        || count(observed, IceCheckKind::kRemoteAddressUnmatched) == 0) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms); observed = host.diagnostics();
    }
    EXPECT_GE(count(observed, IceCheckKind::kNonStunPreselection), 100U);
    EXPECT_GT(count(observed, IceCheckKind::kRemoteAddressUnmatched), 0U);
    EXPECT_EQ(count(observed, IceCheckKind::kRequestRx), 0U);
    EXPECT_EQ(count(observed, IceCheckKind::kValidationFailed), 0U);
    EXPECT_EQ(count(observed, IceCheckKind::kSocketRxServer), 0U);
    EXPECT_LE(observed.recent_events.size(), 64U);
    EXPECT_LT(observed.event_sequence - before.event_sequence, 40U);
    const auto prior_generation = observed.ice_check_generation;
    host.close();
    ASSERT_TRUE(host.startGathering(config));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return candidates >= 2; })); }
    const auto fresh = host.diagnostics();
    EXPECT_GT(fresh.ice_check_generation, prior_generation);
    EXPECT_EQ(count(fresh, IceCheckKind::kNonStunPreselection), 0U);
    EXPECT_EQ(count(fresh, IceCheckKind::kRemoteAddressUnmatched), 0U);
    EXPECT_EQ(count(fresh, IceCheckKind::kSocketRx), 0U);
    ASSERT_TRUE(host.shutdown());
}

TEST(FilteredIce, DiscoveryResponsesDoNotMasqueradeAsPeerIngress) {
    LocalDiscoveryServer server;
    IceConnectivityWrapper host;
    IceGatheringConfig config; config.bind_address = "127.0.0.1";
    config.initiate_offer = true; config.ice_servers = {server.uri()};
    ASSERT_TRUE(host.startGathering(config));
    const auto count = [&](IceCheckKind kind) {
        return host.diagnostics().ice_check_totals[static_cast<std::size_t>(kind)];
    };
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (count(IceCheckKind::kStunServerResponse) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    EXPECT_GT(count(IceCheckKind::kSocketRx), 0U);
    EXPECT_GT(count(IceCheckKind::kSocketRxServer), 0U);
    EXPECT_GT(count(IceCheckKind::kStunServerResponse), 0U);
    EXPECT_EQ(count(IceCheckKind::kSocketRxPeer), 0U);
    EXPECT_EQ(count(IceCheckKind::kResponseRx), 0U);
    EXPECT_EQ(count(IceCheckKind::kStunParseFailed), 0U);
    ASSERT_TRUE(host.shutdown());
}

TEST(FilteredIce, ClosedLoopbackPeerCountsIgnoredUdpErrorWithoutCredentialFailure) {
    std::mutex mutex;
    std::condition_variable cv;
    std::string offer, candidate, mid;
    IceConnectivityWrapper host, controller;
    host.onLocalDescription([&](const auto& sdp, bool) {
        std::lock_guard lock(mutex); offer = sdp; cv.notify_all();
    });
    host.onLocalCandidate([&](const auto& text, const auto& media) {
        std::lock_guard lock(mutex); candidate = text; mid = media; cv.notify_all();
    });
    IceGatheringConfig config; config.bind_address = "127.0.0.1"; config.initiate_offer = true;
    ASSERT_TRUE(host.startGathering(config));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return !offer.empty() && !candidate.empty(); })); }
    ASSERT_TRUE(host.shutdown()); // send to its now-closed UDP port, no firewall manipulation
    config.initiate_offer = false; config.initial_check_window_ms = 3000;
    ASSERT_TRUE(controller.startGathering(config));
    ASSERT_TRUE(controller.applyRemoteDescription(description_without_candidates(offer, false), true));
    ASSERT_TRUE(controller.applyRemoteCandidate(candidate, mid));
    const auto count = [&](IceCheckKind kind) {
        return controller.diagnostics().ice_check_totals[static_cast<std::size_t>(kind)];
    };
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (count(IceCheckKind::kUdpIgnoredConnReset) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    EXPECT_GT(count(IceCheckKind::kUdpErrorObservationReady), 0U);
    EXPECT_GT(count(IceCheckKind::kUdpIgnoredConnReset), 0U);
    EXPECT_EQ(count(IceCheckKind::kSocketIoFailed), 0U);
    EXPECT_EQ(count(IceCheckKind::kValidationFailed), 0U);
    EXPECT_EQ(count(IceCheckKind::kRequestRx), 0U);
    EXPECT_FALSE(controller.diagnostics().candidate_events.empty());
    ASSERT_TRUE(controller.shutdown());
}
TEST(FilteredIce, DhtCandidatesBeforeExplicitAnswerThroughFilteredMappings) {
    FilteredMappings mappings(false);
    std::mutex mutex;
    std::condition_variable cv;
    std::string offer, answer, hc, cc, hm, cm;
    unsigned opened = 0;
    IceConnectivityWrapper controller, host;
    host.onLocalDescription([&](const auto& sdp, bool is_offer) {
        if (is_offer) { std::lock_guard lock(mutex); offer = description_without_candidates(sdp, false); cv.notify_all(); }
    });
    controller.onLocalDescription([&](const auto& sdp, bool is_offer) {
        if (!is_offer) { std::lock_guard lock(mutex); answer = description_without_candidates(sdp, false); cv.notify_all(); }
    });
    host.onLocalCandidate([&](const auto& text, const auto& mid) {
        const auto translated = mappings.translate(0, text);
        if (!translated.empty()) { std::lock_guard lock(mutex); hc = translated; hm = mid; cv.notify_all(); }
    });
    controller.onLocalCandidate([&](const auto& text, const auto& mid) {
        const auto translated = mappings.translate(1, text);
        if (!translated.empty()) { std::lock_guard lock(mutex); cc = translated; cm = mid; cv.notify_all(); }
    });
    auto on_open = [&](DataChannelKind kind) {
        if (kind == DataChannelKind::kMedia || kind == DataChannelKind::kControl) {
            std::lock_guard lock(mutex); ++opened; cv.notify_all();
        }
    };
    host.onDataChannelOpen(on_open);
    controller.onDataChannelOpen(on_open);
    IceGatheringConfig config;
    config.bind_address = "127.0.0.1";
    config.initial_check_window_ms = 120000;
    config.data_channels = {DataChannelKind::kMedia, DataChannelKind::kControl};
    config.initiate_offer = false;
    ASSERT_TRUE(controller.startGathering(config));
    config.initiate_offer = true;
    ASSERT_TRUE(host.startGathering(config));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return !offer.empty() && !hc.empty(); })); }
    ASSERT_TRUE(controller.applyRemoteDescription(offer, true, nullptr, false));
    ASSERT_TRUE(controller.applyRemoteCandidate(hc, hm));
    ASSERT_TRUE(controller.emitLocalDescription());
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return !answer.empty() && !cc.empty(); })); }
    // Bounded local delay: allow early checks to meet the outbound filter.
    std::this_thread::sleep_for(3s);
    ASSERT_TRUE(host.applyRemoteDescription(answer, false));
    ASSERT_TRUE(host.applyRemoteCandidate(cc, cm));
    { std::unique_lock lock(mutex); EXPECT_TRUE(cv.wait_for(lock, 10s, [&] { return opened == 4; })); }
    EXPECT_TRUE(controller.shutdown());
    EXPECT_TRUE(host.shutdown());
    RecordProperty("required_channel_open_events", opened);
    RecordProperty("filtered_packets", mappings.filtered.load());
    RecordProperty("forwarded_packets", mappings.forwarded.load());
    EXPECT_EQ(mappings.errors.load(), 0U);
}
#else
TEST(FilteredIce, WindowsLoopbackFixture) { GTEST_SKIP() << "Windows socket fixture"; }
#endif
} // namespace
