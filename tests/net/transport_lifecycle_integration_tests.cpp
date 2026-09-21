#include "redclaw/net/net_module.h"
#include <rtc/global.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace {
using namespace redclaw::net;
using namespace std::chrono_literals;

class DelayedAnswerTransport : public ::testing::TestWithParam<int> {};

TEST(TransportLifetime, RetirementDrainsHeldDescriptionBeforePublicationReset) {
    for (unsigned cycle = 0; cycle < 5; ++cycle) {
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false, release = false, written = false, retained_old = false;
        unsigned callbacks = 0, ignored_new = 0;
        IceConnectivityWrapper wrapper;
        wrapper.onLocalDescription([&](const auto&, bool offer) {
            if (!offer) return;
            std::unique_lock lock(mutex);
            const auto serial = ++callbacks;
            if (serial == 1) {
                entered = true; cv.notify_all();
                // Bounded even when an assertion fails; no stranded test callback.
                (void)cv.wait_for(lock, 5s, [&] { return release; });
            }
            if (written) {
                if (serial > 1) ++ignored_new;
            } else { written = true; retained_old = serial == 1; }
            cv.notify_all();
        });
        IceGatheringConfig config;
        config.bind_address = "127.0.0.1";
        config.data_channels = {DataChannelKind::kControl};
        auto initial = std::async(std::launch::async, [&] { return wrapper.startGathering(config); });
        { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return entered; })); }
        auto retire = std::async(std::launch::async, [&] { return wrapper.retireAndDrain(); });
        EXPECT_EQ(retire.wait_for(100ms), std::future_status::timeout);
        { std::lock_guard lock(mutex); release = true; cv.notify_all(); }
        ASSERT_TRUE(retire.get());
        (void)initial.get(); // May be superseded while its description callback is held.
        {
            std::lock_guard lock(mutex);
            EXPECT_TRUE(written);
            written = false; retained_old = false;
        }
        ASSERT_TRUE(wrapper.startGathering(config));
        { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return callbacks >= 2; })); }
        EXPECT_TRUE(wrapper.retireAndDrain());
        EXPECT_EQ(ignored_new, 0U);
        EXPECT_FALSE(retained_old);
        EXPECT_TRUE(written);
        EXPECT_TRUE(wrapper.shutdown());
        EXPECT_FALSE(wrapper.retireAndDrain());
        EXPECT_FALSE(wrapper.startGathering(config));
    }
}

TEST(TransportLifetime, RetirementFromCallbackIsRejectedWithoutBlockingOrClosing) {
    IceConnectivityWrapper wrapper;
    bool rejected = false;
    wrapper.onConnectionStateChanged([&](auto state) {
        if (state == IceConnectionState::kGathering) rejected = !wrapper.retireAndDrain();
    });
    IceGatheringConfig config;
    config.bind_address = "127.0.0.1"; config.initiate_offer = false;
    ASSERT_TRUE(wrapper.startGathering(config));
    EXPECT_TRUE(rejected);
    EXPECT_NE(wrapper.connection_state(), IceConnectionState::kClosed);
    EXPECT_TRUE(wrapper.retireAndDrain());
}

TEST(TransportLifetime, CandidateResultDoesNotDependOnErrorTextOrOutputPointer) {
    using Outcome = RemoteCandidateApplyOutcome;
    const std::string candidate = "candidate:1 1 udp 2122260223 127.0.0.1 50000 typ host";
    std::mutex mutex;
    std::condition_variable cv;
    std::string offer;
    IceConnectivityWrapper controller, host;
    std::string error = "arbitrary previous error";
    EXPECT_EQ(controller.tryApplyRemoteCandidate(candidate, "0", &error), Outcome::kNotReady);
    EXPECT_EQ(controller.tryApplyRemoteCandidate(candidate, "0"), Outcome::kNotReady);
    EXPECT_EQ(controller.tryApplyRemoteCandidate("", "0", &error), Outcome::kInvalid);
    EXPECT_EQ(controller.tryApplyRemoteCandidate(candidate, "bad mid"), Outcome::kInvalid);
    host.onLocalDescription([&](const auto& text, bool is_offer) {
        if (is_offer) { std::lock_guard lock(mutex); offer = text; cv.notify_all(); }
    });
    IceGatheringConfig config;
    config.bind_address = "127.0.0.1"; config.initiate_offer = false;
    ASSERT_TRUE(controller.startGathering(config));
    EXPECT_EQ(controller.tryApplyRemoteCandidate(candidate, "0"), Outcome::kNotReady);
    config.initiate_offer = true;
    ASSERT_TRUE(host.startGathering(config));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return !offer.empty(); })); }
    ASSERT_TRUE(controller.applyRemoteDescription(offer, true, nullptr, false));
    EXPECT_EQ(controller.tryApplyRemoteCandidate(candidate, "0", &error), Outcome::kApplied);
    EXPECT_TRUE(error.empty());
    ASSERT_TRUE(controller.retireAndDrain());
    EXPECT_EQ(controller.tryApplyRemoteCandidate(candidate, "0", &error), Outcome::kClosed);
    EXPECT_EQ(controller.tryApplyRemoteCandidate(candidate, "0"), Outcome::kClosed);
    config.initiate_offer = false;
    ASSERT_TRUE(controller.startGathering(config));
    EXPECT_EQ(controller.tryApplyRemoteCandidate(candidate, "0"), Outcome::kNotReady);
    EXPECT_TRUE(controller.shutdown());
    EXPECT_EQ(controller.tryApplyRemoteCandidate(candidate, "0"), Outcome::kClosed);
    EXPECT_TRUE(host.shutdown());
}

TEST_P(DelayedAnswerTransport, OpensRequiredChannelsOrExpiresAtHardDeadline) {
    // Keep only fixed diagnostic categories, never raw SDP/native log messages.
    struct Probe {
        std::atomic<unsigned> failures{0}, starts{0}, finishes{0}, timeouts{0};
        std::atomic<std::int64_t> first_start_ms{-1}, last_start_ms{-1}, timeout_ms{-1};
        std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
    };
    auto probe = std::make_shared<Probe>();
    rtc::InitLogger(rtc::LogLevel::Debug, [probe](auto, const std::string& message) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - probe->began).count();
        if (message.find("Starting DTLS transport") != std::string::npos) {
            if (probe->starts++ == 0) probe->first_start_ms = elapsed;
            probe->last_start_ms = elapsed;
        }
        if (message.find("DTLS handshake failed") != std::string::npos) ++probe->failures;
        if (message.find("DTLS handshake finished") != std::string::npos) ++probe->finishes;
        if (message.find("Handshake timeout") != std::string::npos) {
            ++probe->timeouts; probe->timeout_ms = elapsed;
        }
    });
    struct LoggerReset { ~LoggerReset() { rtc::InitLogger(rtc::LogLevel::None); } } logger_reset;
    std::mutex mutex;
    std::condition_variable cv;
    std::string pending_answer;
    std::vector<std::pair<std::string, std::string>> pending_candidates;
    unsigned opened = 0;
    bool terminal = false;
    IceConnectivityWrapper receiver, host;
    receiver.onConnectionStateChanged([&](auto state) {
        if (state == IceConnectionState::kFailed || state == IceConnectionState::kClosed) {
            std::lock_guard lock(mutex); terminal = true; cv.notify_all();
        }
    });
    receiver.onLocalCandidate([&](const auto& candidate, const auto& mid) {
        std::lock_guard lock(mutex); pending_candidates.emplace_back(candidate, mid); cv.notify_all();
    });
    host.onLocalCandidate([&](const auto& candidate, const auto& mid) { (void)receiver.applyRemoteCandidate(candidate, mid); });
    receiver.onLocalDescription([&](const auto& sdp, bool offer) {
        if (!offer) { std::lock_guard lock(mutex); pending_answer = sdp; cv.notify_all(); }
    });
    host.onLocalDescription([&](const auto& sdp, bool offer) { (void)receiver.applyRemoteDescription(sdp, offer); });
    auto on_open = [&](auto kind) {
        if (kind == DataChannelKind::kControl || kind == DataChannelKind::kMedia) {
            std::lock_guard lock(mutex); ++opened; cv.notify_all();
        }
    };
    receiver.onDataChannelOpen(on_open);
    host.onDataChannelOpen(on_open);
    IceGatheringConfig receive_config;
    receive_config.initiate_offer = false;
    ASSERT_TRUE(receiver.startGathering(receive_config));
    IceGatheringConfig host_config;
    host_config.data_channels = {DataChannelKind::kMedia, DataChannelKind::kControl};
    ASSERT_TRUE(host.startGathering(host_config));
    std::string answer;
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return !pending_answer.empty() && !pending_candidates.empty(); }));
        answer = pending_answer;
    }
    if (GetParam() >= 0) {
        // Includes the 22-second production failure and longer signaling delays.
        std::this_thread::sleep_for(std::chrono::seconds(GetParam()));
        ASSERT_TRUE(host.applyRemoteDescription(answer, false));
        std::vector<std::pair<std::string, std::string>> candidates;
        { std::lock_guard lock(mutex); candidates = pending_candidates; }
        ASSERT_FALSE(candidates.empty());
        for (const auto& [candidate, mid] : candidates) ASSERT_TRUE(host.applyRemoteCandidate(candidate, mid));
        { std::unique_lock lock(mutex); EXPECT_TRUE(cv.wait_for(lock, 20s, [&] { return opened == 4; })); }
        EXPECT_EQ(probe->failures.load(), 0U);
        EXPECT_EQ(probe->finishes.load(), 2U);
    } else {
        std::unique_lock lock(mutex);
        EXPECT_TRUE(cv.wait_for(lock, 125s, [&] { return terminal; }));
        EXPECT_EQ(opened, 0U);
        EXPECT_GE(probe->timeout_ms.load() - probe->first_start_ms.load(), 119000);
        EXPECT_LE(probe->timeout_ms.load() - probe->first_start_ms.load(), 125000);
    }
    EXPECT_TRUE(receiver.shutdown());
    EXPECT_TRUE(host.shutdown());
    RecordProperty("native_dtls_handshake_failures", probe->failures.load());
    RecordProperty("native_dtls_starts", probe->starts.load());
    RecordProperty("native_dtls_finishes", probe->finishes.load());
    RecordProperty("native_dtls_timeouts", probe->timeouts.load());
    RecordProperty("native_dtls_first_start_ms", std::to_string(probe->first_start_ms.load()));
    RecordProperty("native_dtls_last_start_ms", std::to_string(probe->last_start_ms.load()));
    RecordProperty("native_dtls_timeout_ms", std::to_string(probe->timeout_ms.load()));
    unsigned host_ice_connected = 0, host_ice_completed = 0;
    for (const auto& event : host.diagnostics().recent_events) {
        if (event.layer != TransportDiagnosticLayer::kIce) continue;
        if (event.native_state == 2) ++host_ice_connected;
        if (event.native_state == 3) ++host_ice_completed;
    }
    RecordProperty("host_native_ice_connected_events", host_ice_connected);
    RecordProperty("host_native_ice_completed_events", host_ice_completed);
}

INSTANTIATE_TEST_SUITE_P(DhtSignaling, DelayedAnswerTransport,
    ::testing::Values(0, 22, 45, 90, -1), [](const auto& info) {
        return info.param < 0 ? "NeverDelivered" : "Delay" + std::to_string(info.param) + "Seconds";
    });

TEST(TransportLifetime, ConcurrentSendStatsCloseAndRecreateAreGenerationBounded) {
    std::atomic<bool> running{true};
    std::atomic<unsigned> reads{0};
    IceConnectivityWrapper wrapper;
    IceGatheringConfig config;
    config.initiate_offer = false;
    ASSERT_TRUE(wrapper.startGathering(config));
    std::thread reader([&] {
        while (running) {
            DataChannelTransportStats stats;
            (void)wrapper.getDataChannelTransportStats(DataChannelKind::kMedia, &stats);
            (void)wrapper.sendDataChannelMessage(DataChannelKind::kControl, "fixture");
            (void)wrapper.connection_state();
            ++reads;
            std::this_thread::yield();
        }
    });
    for (int i = 0; i < 30; ++i) {
        wrapper.close();
        EXPECT_TRUE(wrapper.startGathering(config));
    }
    running = false;
    reader.join();
    EXPECT_TRUE(wrapper.shutdown());
    EXPECT_FALSE(wrapper.startGathering(config));
    EXPECT_GT(reads, 0U);
    EXPECT_LE(wrapper.diagnostics().recent_events.size(), 64U);
}

TEST(TransportLifetime, AgentOnlyRecreationPreservesRequiredChannels) {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned opened = 0, agent_closed = 0, required_closed = 0;
    unsigned received = 0, terminal = 0;
    IceConnectivityWrapper receiver, host;
    receiver.onLocalCandidate([&](const auto& candidate, const auto& mid) { (void)host.applyRemoteCandidate(candidate, mid); });
    host.onLocalCandidate([&](const auto& candidate, const auto& mid) { (void)receiver.applyRemoteCandidate(candidate, mid); });
    receiver.onLocalDescription([&](const auto& sdp, bool offer) { (void)host.applyRemoteDescription(sdp, offer); });
    host.onLocalDescription([&](const auto& sdp, bool offer) { (void)receiver.applyRemoteDescription(sdp, offer); });
    auto on_open = [&](auto) { std::lock_guard lock(mutex); ++opened; cv.notify_all(); };
    auto on_close = [&](auto kind) {
        std::lock_guard lock(mutex);
        if (kind == DataChannelKind::kAgent) ++agent_closed; else ++required_closed;
        cv.notify_all();
    };
    auto on_state = [&](auto state) {
        if (state == IceConnectionState::kFailed || state == IceConnectionState::kClosed) {
            std::lock_guard lock(mutex); ++terminal; cv.notify_all();
        }
    };
    receiver.onConnectionStateChanged(on_state);
    host.onConnectionStateChanged(on_state);
    receiver.onDataChannelOpen(on_open);
    host.onDataChannelOpen(on_open);
    receiver.onDataChannelClosed(on_close);
    host.onDataChannelClosed(on_close);
    receiver.onDataChannelMessage([&](auto, const auto& message) {
        if (message == "channel_isolation_fixture") {
            std::lock_guard lock(mutex); ++received; cv.notify_all();
        }
    });
    IceGatheringConfig receive_config;
    receive_config.initiate_offer = false;
    ASSERT_TRUE(receiver.startGathering(receive_config));
    IceGatheringConfig host_config;
    host_config.data_channels = {DataChannelKind::kMedia, DataChannelKind::kControl, DataChannelKind::kAgent};
    ASSERT_TRUE(host.startGathering(host_config));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 15s, [&] { return opened == 6; })); }
    for (unsigned cycle = 1; cycle <= 3; ++cycle) {
        ASSERT_TRUE(host.closeDataChannel(DataChannelKind::kAgent));
        { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return agent_closed == cycle * 2; })); }
        ASSERT_TRUE(host.sendDataChannelMessage(DataChannelKind::kControl, "channel_isolation_fixture"));
        ASSERT_TRUE(host.sendDataChannelMessage(DataChannelKind::kMedia, "channel_isolation_fixture"));
        { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return received == cycle * 3 - 1; })); }
        ASSERT_TRUE(host.ensureDataChannel(DataChannelKind::kAgent));
        { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return opened == 6 + cycle * 2; })); }
        ASSERT_TRUE(host.sendDataChannelMessage(DataChannelKind::kAgent, "channel_isolation_fixture"));
        { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return received == cycle * 3; })); }
    }
    {
        std::lock_guard lock(mutex);
        EXPECT_EQ(required_closed, 0U);
        EXPECT_EQ(terminal, 0U);
    }
    EXPECT_TRUE(host.shutdown());
    EXPECT_TRUE(receiver.shutdown());
}

TEST(TransportLifetime, RealChannelCallbackReentryAndOwnerDrain) {
    std::mutex mutex;
    std::condition_variable cv;
    bool host_open = false, receiver_open = false, entered = false, release = false;
    bool callback_shutdown_drained = true;
    IceConnectivityWrapper receiver, host;
    receiver.onLocalCandidate([&](const auto& candidate, const auto& mid) { (void)host.applyRemoteCandidate(candidate, mid); });
    host.onLocalCandidate([&](const auto& candidate, const auto& mid) { (void)receiver.applyRemoteCandidate(candidate, mid); });
    receiver.onLocalDescription([&](const auto& sdp, bool offer) { (void)host.applyRemoteDescription(sdp, offer); });
    host.onLocalDescription([&](const auto& sdp, bool offer) { (void)receiver.applyRemoteDescription(sdp, offer); });
    receiver.onDataChannelOpen([&](auto kind) {
        if (kind != DataChannelKind::kControl) return;
        { std::lock_guard lock(mutex); receiver_open = true; }
        cv.notify_all();
    });
    host.onDataChannelOpen([&](auto kind) {
        if (kind != DataChannelKind::kControl) return;
        { std::lock_guard lock(mutex); host_open = true; }
        cv.notify_all();
    });
    receiver.onDataChannelMessage([&](auto, const auto&) {
        DataChannelTransportStats stats;
        (void)receiver.getDataChannelTransportStats(DataChannelKind::kControl, &stats);
        callback_shutdown_drained = receiver.shutdown(); // Must not wait for itself.
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        (void)cv.wait_for(lock, 5s, [&] { return release; });
    });
    IceGatheringConfig receive_config;
    receive_config.initiate_offer = false;
    ASSERT_TRUE(receiver.startGathering(receive_config));
    IceGatheringConfig host_config;
    host_config.data_channels = {DataChannelKind::kControl};
    ASSERT_TRUE(host.startGathering(host_config));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 15s, [&] { return receiver_open && host_open; })); }
    ASSERT_TRUE(host.sendDataChannelMessage(DataChannelKind::kControl, "drain_fixture"));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 5s, [&] { return entered; })); }
    auto drain = std::async(std::launch::async, [&] { return receiver.shutdown(); });
    EXPECT_EQ(drain.wait_for(100ms), std::future_status::timeout);
    { std::lock_guard lock(mutex); release = true; }
    cv.notify_all();
    EXPECT_EQ(drain.wait_for(5s), std::future_status::ready);
    EXPECT_TRUE(drain.get());
    EXPECT_FALSE(callback_shutdown_drained);
    EXPECT_TRUE(host.shutdown());
    const auto diagnostics = receiver.diagnostics();
    EXPECT_LE(diagnostics.recent_events.size(), 64U);
    EXPECT_EQ(diagnostics.callback_failures, 0U);
    EXPECT_TRUE(std::any_of(diagnostics.recent_events.begin(), diagnostics.recent_events.end(),
        [](const auto& e) { return e.layer == TransportDiagnosticLayer::kIce; }));
}
} // namespace
