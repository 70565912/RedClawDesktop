#include "redclaw/net/video_frame_transport.h"
#include "redclaw/session/frame_cadence.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>

namespace {
using namespace redclaw::net;

TEST(AdaptiveDelivery, CompressionIdleAndProbeAttribution) {
    MediaTransportEstimator estimator;
    MediaTransportFeedbackBatch feedback{1, 1, {}};
    for (std::uint64_t i = 1; i <= 6; ++i) {
        SentMediaTransportPacket packet{i, 1, 1000000 + i * 10000, 1, 10000};
        packet.probe_generation = 7;
        packet.application_limited = i == 1;
        ASSERT_TRUE(estimator.record_sent(packet));
        feedback.arrivals.push_back({i, 9000000 + i * 1000}); // compressed arrivals
    }
    ASSERT_TRUE(estimator.apply_feedback(feedback, 1100000, 1));
    auto estimate = estimator.snapshot(1100000, 15);
    EXPECT_TRUE(estimate.delivery_rate_valid);
    EXPECT_FALSE(estimate.application_limited);
    EXPECT_EQ(estimate.delivery_bitrate_kbps, 8000U);
    EXPECT_EQ(estimate.probe_delivery_bitrate_kbps, 8000U);
    EXPECT_EQ(estimate.probe_generation, 7U);
    EXPECT_EQ(estimate.probe_acknowledged_bytes, 50000U); // exclude first packet boundary
    SentMediaTransportPacket idle{7, 2, 2100000, 1, 1000};
    idle.application_limited = true;
    ASSERT_TRUE(estimator.record_sent(idle));
    ASSERT_TRUE(estimator.apply_feedback({2, 1, {{7, 10100000}}}, 2200000, 1));
    estimate = estimator.snapshot(2200000, 15);
    EXPECT_TRUE(estimate.application_limited);
    EXPECT_FALSE(estimate.delivery_rate_valid);
    EXPECT_FALSE(estimator.apply_feedback(feedback, 2200001, 1));
    EXPECT_EQ(estimator.snapshot(2200001, 15).latest_acknowledged_sequence, 7U);
    for (std::uint64_t i = 8; i <= 11; ++i) {
        SentMediaTransportPacket p{i, 3, 3000000 + i * 1000, 1, 10000};
        p.probe_generation = 8;
        ASSERT_TRUE(estimator.record_sent(p));
    }
    ASSERT_TRUE(estimator.apply_feedback({3, 1, {{9, 11009000}, {10, 11010000}}}, 3100000, 1));
    ASSERT_TRUE(estimator.apply_feedback({4, 1, {{11, 12011000}}}, 3200000, 1));
    estimate = estimator.snapshot(3200000, 15);
    EXPECT_EQ(estimate.loss_per_mille, 0U); // Short loss window no longer contains the lost first probe packet.
    EXPECT_EQ(estimate.probe_generation, 8U);
    EXPECT_FALSE(estimate.probe_rate_valid); // The entire probe still failed.
    MediaTransportEstimator bounded(2);
    for (std::uint64_t i = 1; i <= 3; ++i) {
        SentMediaTransportPacket p{i, 1, 1000000 + i * 1000, 1, 10000};
        p.probe_generation = 1;
        ASSERT_TRUE(bounded.record_sent(p));
    }
    ASSERT_TRUE(bounded.apply_feedback({1, 1, {{2, 9002000}, {3, 9003000}}}, 1100000, 1));
    EXPECT_FALSE(bounded.snapshot(1100000, 15).probe_rate_valid); // Eviction is not an ACK either.
}

TEST(AdaptivePacing, QuantizedWaitRetainsCreditWithoutIdleBurst) {
    for (const std::uint32_t rate : {750U, 3806U, 34000U}) {
        MediaPacingBudget budget;
        std::uint64_t now = 1000000;
        budget.update_rate(rate, now);
        constexpr std::size_t packet = 16384;
        constexpr unsigned packets = 400;
        const auto first = now;
        for (unsigned i = 0; i < packets; ++i) {
            auto wait = budget.delay_until_available_us(packet, now);
            if (wait) {
                const auto elapsed = ((wait + 15999) / 16000) * 16000;
                budget.observe_wait(wait, elapsed);
                now += elapsed;
            }
            ASSERT_TRUE(budget.consume(packet, now));
        }
        const double ideal_us = (packets * packet - 16464.0) * 8000.0 / rate;
        EXPECT_NEAR(static_cast<double>(now - first), ideal_us, 32000.0);
        RecordProperty("pacing_" + std::to_string(rate) + "_elapsed_us", std::to_string(now - first));
        budget.suspend(now + 10000000);
        EXPECT_LE(budget.lateness_us(), 16000U);
        unsigned immediate = 0;
        while (budget.consume(packet, now + 10000000)) ++immediate;
        EXPECT_LE(immediate, 1U);
    }
}

TEST(AdaptivePacing, RationalCadenceDoesNotAccumulateMillisecondRounding) {
    for (const auto fps : {24U, 30U, 59U, 60U}) {
        redclaw::session::FrameCadence cadence;
        std::uint64_t now = 1000;
        for (unsigned i = 0; i < fps * 120; ++i) {
            cadence.advance(now, fps);
            now = ((cadence.due_ms() + 9) / 10) * 10;
        }
        EXPECT_EQ(cadence.due_ms(), 121000U);
        cadence.advance(500000, fps);
        EXPECT_GT(cadence.due_ms(), 500000U);
        EXPECT_LE(cadence.due_ms(), 500000U + (1000 + fps - 1) / fps);
    }
}

TEST(AdaptivePacing, ProbeBoundaryDoesNotPauseOrdinaryMedia) {
    using namespace std::chrono_literals;
    DesktopMediaSendPacer pacer;
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<MediaPacerFrameEvent> terminal;
    std::atomic<std::uint64_t> packets{0};
    ASSERT_TRUE(pacer.start(
        [](std::span<const std::uint8_t>) {
            return MediaPacerSendResult{.accepted = true};
        },
        [] { return MediaPacerTransportState{.open = true}; },
        [&](const SentMediaTransportPacket&) { ++packets; },
        [&](const MediaPacerFrameEvent& event) {
            if (event.type == MediaPacerFrameEventType::kKeyframeRequired) return;
            {
                std::lock_guard lock(mutex);
                terminal = event;
            }
            ready.notify_one();
        }));
    MediaCongestionDecision policy;
    policy.decision_revision = 1;
    policy.pacing_bitrate_kbps = 12000;
    policy.in_flight_limit_bytes = 512 * 1024;
    policy.feedback_horizon_us = 100000;
    policy.recovery_probe = {
        .generation = 1,
        .phase = MediaRecoveryProbePhase::kProbing,
        .wire_budget_bytes = 2 * 16 * 1024,
        .baseline_rate_kbps = 8000,
        .recovery = false,
    };
    pacer.update_policy(policy, 20, 0);
    PacedEncodedVideoFrame frame;
    frame.frame_id = frame.rate_revision = frame.codec = 1;
    frame.width = 640;
    frame.height = 360;
    frame.target_fps = 30;
    frame.target_bitrate_kbps = 3806;
    frame.keyframe = true;
    frame.payload.assign(100000, 0x55);
    ASSERT_TRUE(pacer.submit(std::move(frame)));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(ready.wait_for(lock, 2s, [&] { return terminal.has_value(); }));
    }
    const auto telemetry = pacer.telemetry();
    pacer.stop();
    ASSERT_EQ(terminal->type, MediaPacerFrameEventType::kSent);
    EXPECT_GT(packets.load(), 2U);
    EXPECT_GT(telemetry.probe_end_sequence, 0U);
    EXPECT_EQ(telemetry.deadline_drops, 0U);
}

TEST(AdaptivePacing, KeyframeHistoryAndReceiverEchoDoNotBuildLatency) {
    using namespace std::chrono_literals;
    DesktopMediaSendPacer pacer;
    std::mutex mutex;
    std::condition_variable ready;
    std::atomic<std::uint64_t> sent{0};
    std::atomic<bool> delay_first{true};
    ASSERT_TRUE(pacer.start(
        [&](std::span<const std::uint8_t>) {
            if (delay_first.exchange(false)) std::this_thread::sleep_for(150ms);
            return MediaPacerSendResult{.accepted = true};
        },
        [] { return MediaPacerTransportState{.open = true}; },
        [](const SentMediaTransportPacket&) {},
        [&](const MediaPacerFrameEvent& event) {
            if (event.type == MediaPacerFrameEventType::kSent) ++sent;
            ready.notify_all();
        },
        [&] { ready.notify_all(); }));
    MediaCongestionDecision policy;
    policy.decision_revision = 1;
    policy.pacing_bitrate_kbps = 100000;
    policy.in_flight_limit_bytes = 512 * 1024;
    policy.feedback_horizon_us = 20000;
    policy.receiver_frame_period_us = 250000;
    pacer.update_policy(policy, 10, 0);
    const auto make_frame = [](std::uint64_t id, bool keyframe) {
        PacedEncodedVideoFrame frame;
        frame.frame_id = id;
        frame.rate_revision = 1;
        frame.codec = 1;
        frame.width = 640;
        frame.height = 360;
        frame.target_fps = 30;
        frame.target_bitrate_kbps = 3806;
        frame.keyframe = keyframe;
        frame.payload.assign(1000, static_cast<std::uint8_t>(id));
        return frame;
    };
    ASSERT_TRUE(pacer.submit(make_frame(1, true)));
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(ready.wait_for(lock, 1s, [&] { return sent.load() == 1; }));
    }
    EXPECT_EQ(pacer.telemetry().queue_target_frames, 1U);
    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t id = 2; id <= 5; ++id) {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(ready.wait_for(lock, 1s, [&] { return pacer.can_accept_frame(); }));
        lock.unlock();
        ASSERT_TRUE(pacer.submit(make_frame(id, false)));
    }
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(ready.wait_for(lock, 1s, [&] { return sent.load() == 5; }));
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    pacer.stop();
    EXPECT_LT(elapsed, 500ms);
}

TEST(AdaptiveController, NoDemandOrUnrelatedAcknowledgementCannotConfirmProbe) {
    MediaCongestionController controller;
    MediaCongestionSample s;
    s.now_steady_ms = 1000;
    s.encoder_target_bitrate_kbps = 3806;
    s.media_channel_open = s.rtt_fresh = true;
    s.smoothed_rtt_ms = 10;
    s.demand.target_fps = 30;
    s.transport.feedback_fresh = s.transport.delivery_rate_valid = true;
    s.transport.application_limited = false;
    s.transport.delivery_bitrate_kbps = 3806;
    s.transport.feedback_interval_us = s.transport.feedback_round_trip_us = 100000;
    for (unsigned i = 1; i <= 20; ++i) {
        s.now_steady_ms += 100;
        s.transport.feedback_sample_id = i;
        EXPECT_FALSE(controller.update(s).probe);
    }
    s.demand.pending_bytes = 200000;
    s.demand.token_limited = true;
    ++s.transport.feedback_sample_id;
    const auto probe = controller.update(s);
    ASSERT_TRUE(probe.probe);
    EXPECT_GT(probe.pacing_bitrate_kbps, s.encoder_target_bitrate_kbps);
    ++s.transport.feedback_sample_id;
    s.transport.acknowledged_packets += 1000;
    EXPECT_EQ(controller.update(s).recovery_probe.phase, MediaRecoveryProbePhase::kProbing);
    s.demand.probe_end_sequence = 9;
    s.transport.latest_acknowledged_sequence = 9;
    s.transport.probe_generation = probe.recovery_probe.generation;
    s.transport.probe_rate_valid = true;
    s.transport.probe_delivery_bitrate_kbps = probe.pacing_bitrate_kbps;
    ++s.transport.feedback_sample_id;
    EXPECT_EQ(controller.update(s).recovery_probe.phase, MediaRecoveryProbePhase::kConfirmed);
}

// The virtual network generates packet arrival spans from capacity; it does
// not echo the controller's requested rate as a successful probe result.
TEST(AdaptiveController, GeneratedPathsConvergeDrainAndRecoverWithoutRetuning) {
    for (unsigned seed = 1; seed <= 12; ++seed) {
        SCOPED_TRACE(seed);
        std::mt19937 rng(seed);
        const std::uint32_t high = 26000 + rng() % 70000;
        const std::uint32_t low = 1000 + rng() % 5000;
        const std::uint32_t rtt = 5 + rng() % 250;
        MediaTransportEstimator estimator;
        MediaCongestionController controller;
        MediaCongestionSample s;
        s.encoder_target_bitrate_kbps = 3806;
        s.media_channel_open = s.rtt_fresh = true;
        s.smoothed_rtt_ms = rtt;
        s.demand = {.pending_bytes = 512 * 1024, .target_fps = 30, .token_limited = true};
        std::uint64_t now = 1000000, sequence = 0;
        s.now_steady_ms = now / 1000;
        auto d = controller.update(s);
        std::uint32_t highest = d.pacing_bitrate_kbps, lowest = highest;
        for (unsigned round = 1; round <= 180; ++round) {
            const auto base_capacity = round > 60 && round <= 110 ? low : high;
            // Continuous capacity drift plus a competing flow, without any
            // per-path controller tuning. Abrupt drop/recovery remain above.
            const double competition = round % 31 >= 20 ? 0.7 : 1.0;
            const auto capacity = static_cast<std::uint32_t>(base_capacity * competition
                * (1.0 + 0.15 * std::sin(round * 0.2)));
            s.smoothed_rtt_ms = rtt + static_cast<std::uint32_t>(rtt * 0.2 * (1 + std::sin(round * 0.13)));
            const auto send_gap = std::max<std::uint64_t>(1, 16384ULL * 8000 / d.pacing_bitrate_kbps);
            const auto receive_gap = std::max(send_gap, 16384ULL * 8000 / capacity);
            const bool probing = d.recovery_probe.phase == MediaRecoveryProbePhase::kProbing;
            const auto packets = probing ? std::clamp<std::size_t>(d.recovery_probe.wire_budget_bytes / 16384, 2, 8) : 8;
            MediaTransportFeedbackBatch feedback{round, 1, {}};
            const auto lost_packet = rng() % 23 == 0 ? rng() % packets : packets;
            for (std::size_t p = 0; p < packets; ++p) {
                SentMediaTransportPacket sent{++sequence, round, now + (p + 1) * send_gap, 1, 16384};
                sent.application_limited = p == 0;
                sent.probe_generation = probing ? d.recovery_probe.generation : 0;
                ASSERT_TRUE(estimator.record_sent(sent));
                // Sparse independent loss blocks probes; congestion loss is
                // accompanied by capacity/queue evidence, not a fixed loss %.
                if (p != lost_packet)
                    feedback.arrivals.push_back({sequence, 90000000 + now + (p + 1) * receive_gap});
            }
            // Vary reverse-path aggregation/jitter independently of delivery.
            now += packets * receive_gap + s.smoothed_rtt_ms * 1000ULL
                + rng() % (1000 + s.smoothed_rtt_ms * 500);
            ASSERT_TRUE(estimator.apply_feedback(feedback, now, 1));
            s.transport = estimator.snapshot(now, s.smoothed_rtt_ms);
            s.now_steady_ms = now / 1000;
            s.rtt_sample_id = round;
            s.rtt_queue_delay_ms = static_cast<std::uint32_t>((receive_gap - send_gap) * packets / 1000);
            s.demand.probe_end_sequence = probing ? sequence : 0;
            d = controller.update(s);
            EXPECT_GT(d.pacing_bitrate_kbps, 0U);
            EXPECT_LE(d.in_flight_limit_bytes, 1024U * 1024U);
            if (round <= 60) highest = std::max(highest, d.pacing_bitrate_kbps);
            if (round > 60 && round <= 110) lowest = std::min(lowest, d.pacing_bitrate_kbps);
        }
        EXPECT_GT(highest, 3806U);
        EXPECT_LT(lowest, highest);
        EXPECT_GT(d.pacing_bitrate_kbps, lowest);
        RecordProperty("path_" + std::to_string(seed) + "_peak_kbps", highest);
    }
}

TEST(AdaptiveController, CongestionFreezesAllowanceAndRecoveryDoesNotUseCollapsedTokenClock) {
    MediaCongestionController controller;
    MediaCongestionSample s;
    s.encoder_target_bitrate_kbps = 3806;
    s.smoothed_rtt_ms = 10;
    s.rtt_fresh = s.media_channel_open = true;
    s.demand.target_fps = 30;
    s.rtt_queue_delay_ms = 900;
    s.now_steady_ms = 1000;
    std::uint64_t allowance = 0;
    MediaCongestionDecision d;
    for (unsigned i = 1; i <= 12; ++i) {
        s.rtt_sample_id = i;
        d = controller.update(s);
        if (i == 1) allowance = d.queue_allowance_us;
        EXPECT_EQ(d.queue_allowance_us, allowance);
        EXPECT_NE(d.pressure, MediaNetworkPressure::kStable);
        s.now_steady_ms += (d.feedback_horizon_us + 999) / 1000 + 1;
    }
    const auto collapsed = d.pacing_bitrate_kbps;
    s.now_steady_ms += 300000;
    s.rtt_queue_delay_ms = 0;
    ++s.rtt_sample_id;
    s.recovery_budget_blocked = true;
    d = controller.update(s);
    EXPECT_TRUE(d.probe);
    EXPECT_GT(d.pacing_bitrate_kbps, collapsed);
    EXPECT_LE(d.recovery_probe.wire_budget_bytes, 2U * 16384U);
    EXPECT_EQ(d.in_flight_limit_bytes, 16384U);
    EXPECT_FALSE(d.delivery_rate_valid);
}

TEST(AdaptiveController, MediaEventsDoNotReplayAnOldRttOrLocalPressureObservation) {
    for (const bool local : {false, true}) {
        MediaCongestionController controller;
        MediaCongestionSample s;
        s.encoder_target_bitrate_kbps = 3806;
        s.now_steady_ms = 1000;
        s.demand.target_fps = 30;
        s.media_channel_open = s.rtt_fresh = true;
        s.rtt_sample_id = 1;
        s.rtt_queue_delay_ms = local ? 0 : 220;
        s.local_backpressure = local;
        ASSERT_TRUE(controller.update(s).backoff);
        for (std::uint64_t ack = 1; ack <= 8; ++ack) {
            MediaTransportEstimate t;
            t.feedback_fresh = true;
            t.feedback_sample_id = ack;
            const auto decision = controller.on_feedback(t, s.demand, 1000 + ack * 1000);
            EXPECT_FALSE(decision.backoff);
            EXPECT_FALSE(decision.reduce_fps);
            EXPECT_EQ(decision.backoff_count, 1U);
        }
    }
}

}  // namespace
