#include <gtest/gtest.h>
#include <iostream>
#include <limits>
#include "redclaw/net/video_frame_transport.h"

namespace redclaw::net {
TEST(StreamAdaptationRegression, RttIsConsumedOnceAndDirectPressureRemainsImmediate) {
    MediaCongestionController controller;
    MediaCongestionSample sample;
    sample.encoder_target_bitrate_kbps = 20000;
    sample.now_steady_ms = 1000;
    sample.rtt_fresh = true;
    sample.rtt_sample_id = 1;
    sample.rtt_queue_delay_ms = 220;
    auto first = controller.update(sample);
    ASSERT_TRUE(first.backoff);
    EXPECT_FALSE(first.reduce_fps);
    for (int tick = 0; tick < 3; ++tick) {
        sample.now_steady_ms += 500;
        auto duplicate = controller.update(sample);
        EXPECT_FALSE(duplicate.backoff);
        EXPECT_EQ(duplicate.backoff_count, 1U);
    }
    ++sample.rtt_sample_id;
    auto repeated = controller.update(sample);
    EXPECT_TRUE(repeated.reduce_fps);
    --sample.rtt_sample_id;
    EXPECT_FALSE(controller.update(sample).backoff); // An older sample is not new evidence.
    sample.rtt_fresh = false;
    sample.local_backpressure = true;
    for (int tick = 0; tick < 3; ++tick) {
        ++sample.now_steady_ms;
        EXPECT_TRUE(controller.update(sample).reduce_fps);
    }
}

TEST(StreamAdaptationRegression, PressureCapsDoNotStackOrGetOverriddenByRecovery) {
    MediaCongestionDecision congestion;
    congestion.pressure = MediaNetworkPressure::kSevere;
    congestion.reduce_fps = congestion.backoff = true;
    ReceiverDecodeCapacityDecision capacity;
    capacity.target_changed = true;
    capacity.target_fps = 9;
    EXPECT_EQ(select_stream_target_fps(10, true, congestion, capacity), 7U);
    capacity.target_fps = 11;
    EXPECT_EQ(select_stream_target_fps(10, true, congestion, capacity), 7U);
}

TEST(StreamAdaptationRegression, MisalignedLowFpsDecodeRecoversWithinTenAndThirtySeconds) {
    ReceiverDecodeCapacityController controller;
    ReceiverDecodeCapacitySample sample;
    sample.current_target_fps = 1;
    sample.maximum_target_fps = 4;
    sample.window_ms = 1000;
    sample.revision_consistent = sample.source_active = sample.network_stable = true;
    sample.rate_revision = sample.source_revision = 1;
    const unsigned decoded[] = {0, 2, 1, 1, 1};
    unsigned two_at = 0, four_at = 0;
    for (unsigned second = 1; second <= 30; ++second) {
        sample.reassembled_frames = sample.current_target_fps;
        sample.decoded_frames = decoded[(second - 1) % 5] * sample.current_target_fps;
        sample.network_stable = second % 6 != 0; // A missed heartbeat pauses, not erases progress.
        const auto decision = controller.update(sample);
        if (decision.target_changed) {
            sample.current_target_fps = decision.target_fps;
            ++sample.rate_revision;
        }
        if (!two_at && sample.current_target_fps >= 2) two_at = second;
        if (!four_at && sample.current_target_fps >= 4) four_at = second;
    }
    EXPECT_GT(two_at, 0U);
    EXPECT_LE(two_at, 10U);
    EXPECT_GT(four_at, 0U);
    EXPECT_LE(four_at, 30U);
}

TEST(StreamAdaptationRegression, CadenceFreshnessSpansOneSecondAckBoundaryButIsBounded) {
    MediaTransportEstimator estimator;
    ASSERT_TRUE(estimator.record_sent({1, 1, 1000000, 1, 1000, true}));
    ASSERT_TRUE(estimator.record_sent({2, 2, 2000000, 1, 1000, false}));
    ASSERT_TRUE(estimator.apply_feedback({1, 1, {{1, 1001000}, {2, 2001000}}}, 2010000, 1));
    EXPECT_TRUE(estimator.snapshot(3010000, 10).feedback_fresh);
    EXPECT_TRUE(estimator.snapshot(3011000, 10).feedback_fresh);
    EXPECT_FALSE(estimator.snapshot(5011000, 10).feedback_fresh);
}

TEST(StreamAdaptationRegression, SupersededFramesAreNotGrowingDecodeBacklog) {
    ReceiverDecodeCapacityController controller;
    ReceiverDecodeCapacitySample sample;
    sample.current_target_fps = 10;
    sample.maximum_target_fps = 30;
    sample.window_ms = 1000;
    sample.reassembled_frames = 10;
    sample.decoded_frames = 6;
    sample.pending_frames = 0;
    sample.revision_consistent = sample.source_active = sample.network_stable = true;
    for (int tick = 0; tick < 10; ++tick) {
        EXPECT_FALSE(controller.update(sample).pressure);
    }
    controller.reset();
    for (int tick = 0; tick < 10; ++tick) {
        sample.pending_frames = 4 * (tick + 1);
        auto decision = controller.update(sample);
        if (tick == 9) {
            EXPECT_TRUE(decision.target_changed);
            EXPECT_EQ(decision.target_fps, 6U);
        }
    }
}

TEST(StreamAdaptationRegression, LongEmptyWindowAndRevisionChangeDoNotBankRecovery) {
    ReceiverDecodeCapacityController controller;
    ReceiverDecodeCapacitySample sample;
    sample.current_target_fps = 1;
    sample.maximum_target_fps = 4;
    sample.window_ms = 1000;
    sample.revision_consistent = sample.source_active = sample.network_stable = true;
    for (int tick = 0; tick < 90; ++tick) {
        EXPECT_FALSE(controller.update(sample).target_changed);
    }
    sample.reassembled_frames = sample.decoded_frames = 1;
    for (int tick = 0; tick < 4; ++tick) {
        EXPECT_FALSE(controller.update(sample).target_changed);
    }
    ++sample.rate_revision;
    EXPECT_FALSE(controller.update(sample).target_changed);
    sample.hard_pressure = true;
    sample.network_stable = false;
    for (int tick = 0; tick < 30; ++tick) {
        EXPECT_FALSE(controller.update(sample).target_changed);
    }
}
namespace {
testing::AssertionResult acknowledge_timing_packet(
    MediaTransportEstimator& estimator,
    std::uint64_t sequence,
    std::uint64_t send_us,
    std::uint64_t receive_us,
    std::uint64_t revision = 1) {
    if (!estimator.record_sent({sequence, sequence, send_us, revision, 1000, false})) {
        return testing::AssertionFailure() << "record_sent sequence=" << sequence;
    }
    if (!estimator.apply_feedback(
            {sequence, revision, {{sequence, receive_us}}}, send_us + 50000, revision)) {
        return testing::AssertionFailure() << "apply_feedback sequence=" << sequence;
    }
    return testing::AssertionSuccess();
}
}  // namespace

TEST(MediaTransportTiming, LatestAppliedPairSurvivesRetirementWithoutMutatingEstimates) {
    MediaTransportEstimator estimator;
    EXPECT_FALSE(estimator.timing_snapshot().has_value());
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        ASSERT_TRUE(estimator.record_sent({sequence, sequence + 10,
            1000000 + sequence * 10000, 4, 1000, false}));
    }
    const MediaTransportFeedbackBatch feedback{7, 4, {{1, 8000000}, {3, 8030000}}};
    ASSERT_TRUE(estimator.apply_feedback(feedback, 1100000, 4));
    const auto before = estimator.snapshot(1100000, 10);
    ASSERT_EQ(before.in_flight_bytes, 0U);
    ASSERT_EQ(before.lost_packets, 1U);
    ASSERT_EQ(before.acknowledged_packets, 2U);
    for (int read = 0; read < 3; ++read) {
        const auto sample = estimator.timing_snapshot();
        ASSERT_TRUE(sample.has_value());
        EXPECT_EQ(sample->sample_count, 2U); // The missing packet is not a pair.
        EXPECT_EQ(sample->feedback_id, 7U);
        EXPECT_EQ(sample->transport_sequence, 3U);
        EXPECT_EQ(sample->frame_id, 13U);
        EXPECT_EQ(sample->sent_rate_revision, 4U);
        EXPECT_EQ(sample->feedback_rate_revision, 4U);
        EXPECT_EQ(sample->feedback_host_us, 1100000U);
        EXPECT_EQ(sample->steady_send_us, 1030000U);
        EXPECT_EQ(sample->receiver_steady_us, 8030000U);
        EXPECT_EQ(sample->anchor_send_us, 1010000U);
        EXPECT_EQ(sample->anchor_arrival_us, 8000000U);
        EXPECT_DOUBLE_EQ(sample->relative_transit_us, 10000.0);
        EXPECT_DOUBLE_EQ(sample->smoothed_relative_transit_us, 1000.0);
        EXPECT_DOUBLE_EQ(sample->minimum_smoothed_relative_transit_us, 0.0);
        EXPECT_EQ(sample->queue_delay_ms, before.queue_delay_ms);
    }
    const auto after = estimator.snapshot(1100000, 10);
    EXPECT_EQ(after.feedback_sample_id, before.feedback_sample_id);
    EXPECT_EQ(after.queue_delay_ms, before.queue_delay_ms);
    EXPECT_EQ(after.acknowledged_bitrate_kbps, before.acknowledged_bitrate_kbps);
    EXPECT_EQ(after.acknowledged_packets, before.acknowledged_packets);
    EXPECT_EQ(after.loss_per_mille, before.loss_per_mille);
    EXPECT_FALSE(estimator.apply_feedback(feedback, 1200000, 4));
    EXPECT_FALSE(estimator.apply_feedback({8, 4, {}}, 1200000, 4));
    EXPECT_FALSE(estimator.apply_feedback({9, 4, {{4, 0}}}, 1200000, 4));
    ASSERT_TRUE(estimator.timing_snapshot().has_value());
    EXPECT_EQ(estimator.timing_snapshot()->sample_count, 2U);
    EXPECT_EQ(estimator.timing_snapshot()->feedback_id, 7U);
}

TEST(MediaTransportTiming, StaleFeedbackDoesNotOverwritePairAndRevisionIdentityIsExplicit) {
    MediaTransportEstimator estimator;
    ASSERT_TRUE(acknowledge_timing_packet(estimator, 1, 1000000, 8000000, 7));
    ASSERT_TRUE(estimator.record_sent({2, 2, 1100000, 7, 1000, false}));
    ASSERT_TRUE(estimator.record_sent({3, 3, 1200000, 8, 1000, false}));
    EXPECT_FALSE(estimator.apply_feedback({2, 7, {{2, 8100000}}}, 1250000, 8));
    const auto retained = estimator.timing_snapshot();
    ASSERT_TRUE(retained.has_value());
    EXPECT_EQ(retained->sample_count, 1U);
    EXPECT_EQ(retained->feedback_id, 1U);
    ASSERT_TRUE(estimator.apply_feedback({3, 8, {{3, 8200000}}}, 1260000, 8));
    const auto current = estimator.timing_snapshot();
    ASSERT_TRUE(current.has_value());
    EXPECT_EQ(current->sample_count, 2U);
    EXPECT_EQ(current->sent_rate_revision, 8U);
    EXPECT_EQ(current->feedback_rate_revision, 8U);
    EXPECT_EQ(current->anchor_send_us, 1000000U); // Revision change is not an epoch reset.
    ASSERT_TRUE(estimator.record_sent({4, 4, 1300000, 8, 1000, false}));
    ASSERT_TRUE(estimator.apply_feedback({4, 9, {{4, 8300000}}}, 1350000, 9));
    const auto mixed = estimator.timing_snapshot();
    ASSERT_TRUE(mixed.has_value());
    EXPECT_EQ(mixed->sent_rate_revision, 8U);
    EXPECT_EQ(mixed->feedback_rate_revision, 9U);
}

TEST(MediaTransportTiming, EvictionExpiryAndResetCannotFabricateAPair) {
    MediaTransportEstimator estimator(1);
    ASSERT_TRUE(estimator.record_sent({1, 1, 1000000, 1, 1000, false}));
    ASSERT_TRUE(estimator.record_sent({2, 2, 1100000, 1, 1000, false}));
    ASSERT_TRUE(estimator.apply_feedback({1, 1, {{1, 8000000}}}, 1150000, 1));
    EXPECT_FALSE(estimator.timing_snapshot().has_value());
    ASSERT_TRUE(estimator.apply_feedback({2, 1, {{2, 8100000}}}, 1160000, 1));
    ASSERT_TRUE(estimator.timing_snapshot().has_value());
    EXPECT_EQ(estimator.timing_snapshot()->sample_count, 1U);
    EXPECT_EQ(estimator.timing_snapshot()->transport_sequence, 2U);
    estimator.reset();
    EXPECT_FALSE(estimator.timing_snapshot().has_value());
    ASSERT_TRUE(estimator.record_sent({1, 1, 2000000, 2, 1000, false}));
    EXPECT_EQ(estimator.snapshot(20000000, 10).expired_in_flight_packets, 1U);
    ASSERT_TRUE(estimator.apply_feedback({1, 2, {{1, 9000000}}}, 20000001, 2));
    EXPECT_FALSE(estimator.timing_snapshot().has_value());
    ASSERT_TRUE(acknowledge_timing_packet(estimator, 2, 21000000, 28000000, 2));
    const auto fresh = estimator.timing_snapshot();
    ASSERT_TRUE(fresh.has_value());
    EXPECT_EQ(fresh->sample_count, 1U);
    EXPECT_EQ(fresh->anchor_send_us, 21000000U);
    EXPECT_EQ(fresh->anchor_arrival_us, 28000000U);
    EXPECT_DOUBLE_EQ(fresh->relative_transit_us, 0.0);
    EXPECT_EQ(fresh->queue_delay_ms, 0U);
}

TEST(MediaTransportTiming, ConstantClockOffsetDoesNotCreateQueueInEitherDirection) {
    for (const std::uint64_t receive_base : {1000000ULL, 9000000000000ULL}) {
        SCOPED_TRACE(receive_base);
        MediaTransportEstimator estimator;
        for (std::uint64_t sequence = 1; sequence <= 300; ++sequence) {
            ASSERT_TRUE(acknowledge_timing_packet(estimator, sequence,
                4000000000000ULL + sequence * 100000,
                receive_base + sequence * 100000));
        }
        const auto sample = estimator.timing_snapshot();
        ASSERT_TRUE(sample.has_value());
        EXPECT_DOUBLE_EQ(sample->relative_transit_us, 0.0);
        EXPECT_DOUBLE_EQ(sample->smoothed_relative_transit_us, 0.0);
        EXPECT_EQ(sample->queue_delay_ms, 0U);
    }
}

TEST(MediaTransportTiming, ClockRateCharacterizationReproducesFalseQueueWithoutNetworkGrowth) {
    // Characterization of the unchanged algorithm, NOT an assertion that skew
    // has been fixed or measured on the connected machines. Virtual time only.
    constexpr std::uint64_t kSendBaseUs = 4000000000000ULL;
    constexpr std::uint64_t kReceiveBaseUs = 9000000000000ULL;
    constexpr std::uint64_t kDurationUs = 9600000000ULL; // 160 minutes.
    for (const std::int64_t skew_ppm : {0, 30, -30}) {
        SCOPED_TRACE(skew_ppm);
        MediaTransportEstimator estimator;
        std::uint64_t sequence = 0;
        for (std::uint64_t elapsed_us = 0; elapsed_us <= kDurationUs; elapsed_us += 100000) {
            const auto receiver_elapsed = static_cast<std::int64_t>(elapsed_us)
                + static_cast<std::int64_t>(elapsed_us) * skew_ppm / 1000000;
            // Match the existing receiver's millisecond clock quantization.
            const auto receive_us = (kReceiveBaseUs
                + static_cast<std::uint64_t>(receiver_elapsed)) / 1000 * 1000;
            ASSERT_TRUE(acknowledge_timing_packet(estimator, ++sequence,
                kSendBaseUs + elapsed_us, receive_us));
        }
        const auto estimate = estimator.snapshot(kSendBaseUs + kDurationUs + 50000, 10);
        const auto sample = estimator.timing_snapshot();
        ASSERT_TRUE(sample.has_value());
        EXPECT_EQ(sample->sample_count, 96001U);
        EXPECT_EQ(estimate.in_flight_bytes, 0U);
        EXPECT_EQ(estimate.loss_per_mille, 0U);
        EXPECT_TRUE(estimate.feedback_fresh);
        EXPECT_EQ(sample->queue_delay_ms, estimate.queue_delay_ms);
        EXPECT_DOUBLE_EQ(sample->relative_transit_us, static_cast<double>(skew_ppm) * 9600.0);
        if (skew_ppm > 0) {
            EXPECT_GE(estimate.queue_delay_ms, 287U);
            EXPECT_LE(estimate.queue_delay_ms, 288U);
            MediaCongestionController controller;
            MediaCongestionSample congestion;
            congestion.now_steady_ms = (kSendBaseUs + kDurationUs + 50000) / 1000;
            congestion.encoder_target_bitrate_kbps = 20000;
            congestion.transport = estimate;
            congestion.rtt_fresh = true;
            congestion.rtt_sample_id = 1;
            congestion.smoothed_rtt_ms = 10;
            const auto decision = controller.update(congestion);
            EXPECT_TRUE(decision.backoff);
            EXPECT_TRUE(decision.reduce_fps);
            EXPECT_EQ(decision.pressure, MediaNetworkPressure::kSevere);
        } else {
            EXPECT_EQ(estimate.queue_delay_ms, 0U);
        }
        std::cout << "clock_characterization skew_ppm=" << skew_ppm
                  << " duration_s=9600 queue_ms=" << estimate.queue_delay_ms
                  << " relative_us=" << sample->relative_transit_us << '\n';
    }
}

TEST(MediaTransportTiming, GenuineDelayStepRemainsVisibleWithEitherClockRateSign) {
    for (const std::int64_t skew_ppm : {0, 30, -30}) {
        SCOPED_TRACE(skew_ppm);
        MediaTransportEstimator estimator;
        for (std::uint64_t sequence = 1; sequence <= 300; ++sequence) {
            const std::uint64_t elapsed_us = sequence * 100000;
            const std::uint64_t delay_us = sequence >= 100 ? 250000 : 0;
            const auto receive_us = static_cast<std::uint64_t>(8000000
                + static_cast<std::int64_t>(elapsed_us + delay_us)
                + static_cast<std::int64_t>(elapsed_us) * skew_ppm / 1000000);
            ASSERT_TRUE(acknowledge_timing_packet(estimator, sequence,
                1000000 + elapsed_us, receive_us));
        }
        const auto sample = estimator.timing_snapshot();
        ASSERT_TRUE(sample.has_value());
        EXPECT_GE(sample->queue_delay_ms, 249U);
        EXPECT_LE(sample->queue_delay_ms, 251U);
    }
}

TEST(MediaTransportTiming, PeriodicLogIsNumericSingleLineAndFitsRemoteSnapshotChunk) {
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    MediaTransportTimingSample sample;
    sample.sample_count = sample.feedback_id = sample.transport_sequence = kMax;
    sample.frame_id = sample.sent_rate_revision = sample.feedback_rate_revision = kMax;
    sample.feedback_host_us = sample.steady_send_us = sample.receiver_steady_us = kMax;
    sample.anchor_send_us = sample.anchor_arrival_us = kMax;
    sample.relative_transit_us = -static_cast<double>(kMax);
    sample.smoothed_relative_transit_us = static_cast<double>(kMax);
    sample.minimum_smoothed_relative_transit_us = -static_cast<double>(kMax);
    sample.queue_delay_ms = 120000;
    const auto line = format_media_transport_timing_sample(sample, kMax, 4294967295U);
    EXPECT_LT(line.size() + 100U, 1024U); // Reserve GUI timestamp/PID/source prefix.
    EXPECT_EQ(line.find_first_of("\r\n"), std::string::npos);
    EXPECT_NE(line.find("role=host v=1"), std::string::npos);
    EXPECT_NE(line.find("send_us=18446744073709551615"), std::string::npos);
    EXPECT_NE(line.find("recv_us=18446744073709551615"), std::string::npos);
    EXPECT_NE(line.find("ack_host_us="), std::string::npos);
    EXPECT_NE(line.find("log_host_us="), std::string::npos);
    EXPECT_NE(line.find("queue_ms=120000 rtt_queue_latest_ms=4294967295"), std::string::npos);
    sample.relative_transit_us = -0.125;
    EXPECT_NE(format_media_transport_timing_sample(sample, 1, 0).find("relative_us=-0.125"),
        std::string::npos);
}
} // namespace redclaw::net
