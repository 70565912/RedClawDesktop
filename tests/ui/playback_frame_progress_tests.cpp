#include "ui/playback/playback_frame_progress.h"
#include "ui/playback/capture_playback_state.h"
#include <gtest/gtest.h>

TEST(PlaybackFrameProgress, HostRestartCountsLowNumberedNewFramesAndRejectsOldSnapshots) {
    redclaw::ui::PlaybackFrameProgress progress;
    EXPECT_FALSE(progress.observe_source("host-before"));
    ASSERT_TRUE(progress.accept(1, 31000, true));
    EXPECT_TRUE(progress.mark_presented(31000));
    EXPECT_FALSE(progress.mark_presented(31000));
    progress.reported = 31000;
    ASSERT_TRUE(progress.observe_source("host-after"));
    progress.reset_frames(2);
    EXPECT_EQ(progress.reported, 0U);
    EXPECT_FALSE(progress.accept(1, 32000, true));
    progress.observe_transport(1, 32000, 32000);
    EXPECT_EQ(progress.displayable, 0U);
    EXPECT_EQ(progress.presented, 0U);
    EXPECT_TRUE(progress.accept(2, 1, true));
    EXPECT_TRUE(progress.mark_presented(1));
    EXPECT_FALSE(progress.mark_presented(1));
    EXPECT_TRUE(progress.accept(2, 2, false));
    EXPECT_TRUE(progress.mark_presented(2));
    EXPECT_EQ(progress.keyframe, 1U);
    EXPECT_FALSE(progress.observe_source("host-after"));
}

TEST(PlaybackFrameProgress, RestartCaptureStateRequiresAnActuallyPresentedNewFrameAndRearm) {
    redclaw::ui::CapturePlaybackState capture;
    redclaw::protocol::StreamControlMessageV1 state;
    state.capture_status_version = 1; state.capture_status = 1;
    state.capture_generation = 15; state.capture_first_frame_id = 25000;
    capture.observe(state); capture.presented(25000);
    ASSERT_TRUE(capture.request_control());
    capture = {};
    state.capture_generation = 1; state.capture_first_frame_id = 2;
    capture.observe(state);
    EXPECT_TRUE(capture.waiting());
    EXPECT_FALSE(capture.request_control());
    capture.presented(1);
    EXPECT_TRUE(capture.waiting());
    capture.presented(2);
    EXPECT_FALSE(capture.waiting());
    EXPECT_FALSE(capture.control_allowed());
    EXPECT_TRUE(capture.request_control());
}
