#include <gtest/gtest.h>
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
} // namespace redclaw::net
