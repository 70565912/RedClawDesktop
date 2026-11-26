#include "runtime/controller_playback_runtime.h"
#include <gtest/gtest.h>

TEST(ControllerPlaybackRuntimeTest, TimedSmokeNeedsCurrentLocalPresentationNotDecodeOrDisplayability) {
    using Runtime = redclaw::runtime::ControllerPlaybackRuntime;
    const redclaw::helper::PlaybackStateSnapshot current{.session = 12, .revision = 3};
    redclaw::protocol::StreamControlMessageV1 report;
    report.type = redclaw::protocol::StreamControlMessageTypeV1::kReceiverNetworkStats;
    Runtime::stamp_playback_identity(report, current);
    report.decoded_frames = 1;
    report.observed_source_activity_revision = 1;
    report.latest_displayable_frame_id = 8;
    EXPECT_FALSE(Runtime::confirms_presentation(report, current));
    report.latest_presented_frame_id = 8;
    EXPECT_FALSE(Runtime::confirms_presentation(report, current));
    report.rendered_frames = 1;
    EXPECT_TRUE(Runtime::confirms_presentation(report, current));
    report.observed_source_activity_revision = 0;
    report.latest_presented_frame_id = report.latest_displayable_frame_id = 0;
    EXPECT_TRUE(Runtime::confirms_presentation(report, current));
    ++report.playback_session;
    EXPECT_FALSE(Runtime::confirms_presentation(report, current));
}

TEST(ControllerPlaybackRuntimeTest, KeyframeRequestMustMatchActivePlaybackAtSend) {
    using Runtime = redclaw::runtime::ControllerPlaybackRuntime;
    using Type = redclaw::protocol::StreamControlMessageTypeV1;

    const redclaw::helper::PlaybackStateSnapshot submitted{.session = 4, .revision = 9};
    redclaw::protocol::StreamControlMessageV1 request;
    request.type = Type::kKeyframeRequest;
    Runtime::stamp_playback_identity(request, submitted);
    EXPECT_TRUE(Runtime::matches_feedback(request, submitted));

    auto current = submitted;
    ++current.session;
    EXPECT_FALSE(Runtime::matches_feedback(request, current));
    current = submitted;
    ++current.revision;
    EXPECT_FALSE(Runtime::matches_feedback(request, current));
    current = submitted;
    current.paused = true;
    EXPECT_FALSE(Runtime::matches_feedback(request, current));

    Runtime::stamp_playback_identity(request, current);
    EXPECT_FALSE(Runtime::matches_feedback(request, current));
    request.type = Type::kReceiverNetworkStats;
    EXPECT_TRUE(Runtime::matches_feedback(request, current));
}

TEST(ControllerPlaybackRuntimeTest, ManualCaptureRetryBypassesPlaybackIdentityFence) {
    using Runtime = redclaw::runtime::ControllerPlaybackRuntime;
    using Type = redclaw::protocol::StreamControlMessageTypeV1;

    const redclaw::helper::PlaybackStateSnapshot current{.session = 4, .revision = 9};
    redclaw::protocol::StreamControlMessageV1 retry;
    retry.type = Type::kKeyframeRequest;
    retry.capture_retry_requested = true;

    EXPECT_FALSE(Runtime::should_consume_stale_playback_command(retry, current));
    auto paused = current;
    paused.paused = true;
    EXPECT_FALSE(Runtime::should_consume_stale_playback_command(retry, paused));
    auto other = current;
    ++other.session;
    EXPECT_FALSE(Runtime::should_consume_stale_playback_command(retry, other));
    other.valid = false;
    EXPECT_FALSE(Runtime::should_consume_stale_playback_command(retry, other));

    redclaw::protocol::StreamControlMessageV1 request;
    request.type = Type::kKeyframeRequest;
    Runtime::stamp_playback_identity(request, current);
    EXPECT_FALSE(Runtime::should_consume_stale_playback_command(request, current));
    EXPECT_TRUE(Runtime::should_consume_stale_playback_command(request, paused));
    EXPECT_TRUE(Runtime::should_consume_stale_playback_command(request, other));
}
