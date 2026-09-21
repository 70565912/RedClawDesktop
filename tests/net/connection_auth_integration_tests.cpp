#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include "redclaw/net/net_module.h"

namespace {
using namespace redclaw::net;
using redclaw::security::ConnectionAuthState;
using Kind = DataChannelKind;
constexpr std::array kinds{Kind::kMedia, Kind::kControl, Kind::kAgent, Kind::kNavigation,
    Kind::kDebugBridge, Kind::kTerminal, Kind::kTransfer};
class AuthPair {
public:
    IceConnectivityWrapper host, client;
    std::atomic<unsigned> host_open{0}, client_open{0}, host_received{0}, client_received{0};
    std::atomic<bool> native_connected{false};
    std::mutex mutex;
    struct Event { bool host; bool description; std::string value, mid; bool offer; };
    std::deque<Event> events;
    AuthPair(bool new_host, bool new_client, std::string password = "password") {
        std::string verifier; EXPECT_TRUE(redclaw::security::make_connection_verifier("password", &verifier));
        if (new_host) EXPECT_TRUE(host.requireConnectionAuthentication({true, verifier}));
        if (new_client) EXPECT_TRUE(client.requireConnectionAuthentication({false, password}));
        host.onLocalDescription([this](auto sdp, bool offer) { enqueue({false, true, sdp, {}, offer}); });
        client.onLocalDescription([this](auto sdp, bool offer) { enqueue({true, true, sdp, {}, offer}); });
        host.onLocalCandidate([this](auto sdp, auto mid) { enqueue({false, false, sdp, mid, false}); });
        client.onLocalCandidate([this](auto sdp, auto mid) { enqueue({true, false, sdp, mid, false}); });
        host.onConnectionStateChanged([this](auto state) { if (state == IceConnectionState::kConnected) native_connected = true; });
        host.onDataChannelOpen([this, new_host](auto kind) {
            ++host_open; if (!new_host) {
                const std::array<std::uint8_t, 1> premature{42};
                host.sendDataChannelBinaryMessage(kind, premature);
                if (kind == Kind::kControl) legacy_hello(host);
            }
        });
        client.onDataChannelOpen([this, new_client](auto kind) {
            ++client_open; if (!new_client) {
                const std::array<std::uint8_t, 1> premature{42};
                client.sendDataChannelBinaryMessage(kind, premature);
                if (kind == Kind::kControl) legacy_hello(client);
            }
        });
        host.onDataChannelBinaryMessage([this](auto, auto) { ++host_received; });
        client.onDataChannelBinaryMessage([this](auto, auto) { ++client_received; });
    }
    ~AuthPair() { host.shutdown(); client.shutdown(); }
    void enqueue(Event event) { std::lock_guard lock(mutex); events.push_back(std::move(event)); }
    static void legacy_hello(IceConnectivityWrapper& peer) {
        redclaw::protocol::StreamControlMessageV1 message;
        message.session_epoch = "legacy"; message.message_id = 1; message.sent_at_ms = 1;
        const auto bytes = redclaw::protocol::serialize_stream_control_message_v1(message);
        peer.sendDataChannelBinaryMessage(Kind::kControl,
            {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    }
    void start() {
        IceGatheringConfig config; config.initiate_offer = false; config.bind_address = "127.0.0.1";
        config.data_channels.assign(kinds.begin(), kinds.end());
        std::string error; ASSERT_TRUE(client.startGathering(config, &error)) << error;
        config.initiate_offer = true;
        ASSERT_TRUE(host.startGathering(config, &error)) << error;
    }
    bool wait(const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        while (std::chrono::steady_clock::now() < deadline) {
            std::deque<Event> batch; { std::lock_guard lock(mutex); batch.swap(events); }
            for (const auto& event : batch) {
                auto& target = event.host ? host : client;
                std::string error;
                if (event.description) EXPECT_TRUE(target.applyRemoteDescription(event.value, event.offer, &error)) << error;
                else {
                    const auto result = target.tryApplyRemoteCandidate(event.value, event.mid, &error);
                    if (result == RemoteCandidateApplyOutcome::kNotReady) enqueue(event);
                    else EXPECT_EQ(result, RemoteCandidateApplyOutcome::kApplied) << error;
                }
            }
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return done();
    }
};
TEST(ConnectionAuthTransport, CorrectPasswordOpensAllChannelsAndReauthenticationIsRequired) {
    AuthPair pair(true, true);
    for (const auto kind : kinds) EXPECT_FALSE(pair.host.sendDataChannelMessage(kind, "before-auth"));
    pair.start();
    ASSERT_TRUE(pair.wait([&] { return pair.host_open == kinds.size() && pair.client_open == kinds.size(); }));
    EXPECT_EQ(pair.host.authenticationState(), ConnectionAuthState::kAccepted);
    EXPECT_EQ(pair.client.authenticationState(), ConnectionAuthState::kAccepted);
    EXPECT_EQ(pair.host_received, 0U); EXPECT_EQ(pair.client_received, 0U);
    const std::array<std::uint8_t, 1> bytes{42};
    for (const auto kind : kinds) ASSERT_TRUE(pair.host.sendDataChannelBinaryMessage(kind, bytes));
    ASSERT_TRUE(pair.wait([&] { return pair.client_received == kinds.size(); }));
    ASSERT_TRUE(pair.host.retireAndDrain()); ASSERT_TRUE(pair.client.retireAndDrain());
    EXPECT_EQ(pair.host.authenticationState(), ConnectionAuthState::kPending);
    for (const auto kind : kinds) EXPECT_FALSE(pair.host.sendDataChannelBinaryMessage(kind, bytes));
    pair.host_open = 0; pair.client_open = 0;
    pair.start();
    ASSERT_TRUE(pair.wait([&] { return pair.host_open == kinds.size() && pair.client_open == kinds.size(); }));
}
TEST(ConnectionAuthTransport, WrongPasswordExposesNoBusinessChannelOrPayload) {
    AuthPair pair(true, true, "incorrect"); pair.start();
    ASSERT_TRUE(pair.wait([&] { return pair.host.authenticationState() == ConnectionAuthState::kRejected
        && pair.client.authenticationState() == ConnectionAuthState::kRejected; }));
    EXPECT_EQ(pair.host_open, 0U); EXPECT_EQ(pair.client_open, 0U);
    EXPECT_EQ(pair.host_received, 0U); EXPECT_EQ(pair.client_received, 0U);
    for (const auto kind : kinds) {
        EXPECT_FALSE(pair.host.sendDataChannelMessage(kind, "blocked"));
        DataChannelTransportStats stats;
        ASSERT_TRUE(pair.host.getDataChannelTransportStats(kind, &stats)); EXPECT_FALSE(stats.open);
    }
    std::atomic<unsigned> closed{0};
    pair.host.onDataChannelClosed([&](auto) { ++closed; });
    pair.client.shutdown();
    ASSERT_TRUE(pair.wait([&] { return closed > 0; }));
    EXPECT_EQ(pair.host.authenticationState(), ConnectionAuthState::kRejected);
}
TEST(ConnectionAuthTransport, BothLegacyDirectionsRejectWithoutExposingProtectedSide) {
    for (const bool new_host : {true, false}) {
        AuthPair pair(new_host, !new_host); pair.start();
        auto& protected_peer = new_host ? pair.host : pair.client;
        ASSERT_TRUE(pair.wait([&] { return protected_peer.authenticationState() == ConnectionAuthState::kRejected; }));
        std::string error; protected_peer.authenticationState(&error);
        EXPECT_EQ(error, "protocol_version_incompatible");
        EXPECT_EQ(new_host ? pair.host_open.load() : pair.client_open.load(), 0U);
        EXPECT_EQ(new_host ? pair.host_received.load() : pair.client_received.load(), 0U);
    }
}
} // namespace
