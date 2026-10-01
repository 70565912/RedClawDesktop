#include "../../src/net/src/media_pacer_wait.h"
#include "redclaw/net/video_frame_transport.h"

#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <thread>

namespace {
using namespace redclaw::net;
using namespace std::chrono_literals;

// A wait duration includes reacquiring the predicate mutex. A large overshoot
// alone therefore cannot distinguish late scheduling from lock contention.
TEST(SenderTimingProvenance, WaitElapsedIncludesPredicateMutexReacquisition) {
    MediaPacerWait wait(MediaPacerWait::Mode::kConditionVariable);
    std::mutex mutex;
    std::promise<void> entering;
    auto ready = entering.get_future();
    auto result = std::async(std::launch::async, [&] {
        std::unique_lock lock(mutex);
        const auto begin = MediaPacerWait::Clock::now();
        entering.set_value();
        wait.wait_for(lock, 20ms, [] { return false; });
        return std::chrono::duration_cast<std::chrono::microseconds>(
            MediaPacerWait::Clock::now() - begin).count();
    });
    ASSERT_EQ(ready.wait_for(1s), std::future_status::ready);
    // Acquiring this mutex establishes that wait_until has released it.
    { std::lock_guard lock(mutex); std::this_thread::sleep_for(80ms); }
    ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
    const auto elapsed_us = result.get();
    RecordProperty("requested_us", "20000");
    RecordProperty("elapsed_us", std::to_string(elapsed_us));
    EXPECT_GE(elapsed_us, 80000);
}

enum class DelayedRegion { kTransportState, kSend };
class SenderCallbackTiming : public testing::TestWithParam<DelayedRegion> {};

TEST_P(SenderCallbackTiming, DelayedCallbackUsesItsOwnTraceBucketWithoutInFlightWait) {
    const auto region = GetParam();
    MediaFrameTraceRecorder recorder;
    const auto start = media_trace_now_us();
    ASSERT_TRUE(recorder.arm(start, 1));
    std::promise<MediaPacerFrameEvent> completed;
    auto done = completed.get_future();
    DesktopMediaSendPacer pacer;
    ASSERT_TRUE(pacer.start([&](auto) {
        if (region == DelayedRegion::kSend) std::this_thread::sleep_for(80ms);
        return MediaPacerSendResult{.accepted = true};
    }, [&] {
        if (region == DelayedRegion::kTransportState) std::this_thread::sleep_for(80ms);
        return MediaPacerTransportState{.open = true};
    }, [](const auto&) {}, [&](const auto& event) {
        if (event.type != MediaPacerFrameEventType::kKeyframeRequired) completed.set_value(event);
    }, {}, &recorder));
    pacer.update_budget(20000, 9, 0);
    PacedEncodedVideoFrame frame;
    frame.frame_id = 1; frame.rate_revision = 1; frame.codec = 1;
    frame.width = 320; frame.height = 180; frame.target_fps = 30;
    frame.target_bitrate_kbps = 4267; frame.keyframe = true;
    frame.payload.assign(100, 0x55);
    frame.trace.enabled = true; frame.trace.encode_begin_us = start + 1;
    ASSERT_TRUE(pacer.submit(std::move(frame)));
    ASSERT_EQ(done.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(done.get().type, MediaPacerFrameEventType::kSent);
    pacer.stop();
    auto batch = recorder.take_completed(start + 12000000);
    ASSERT_TRUE(batch); ASSERT_EQ(batch->overflow, 0U);
    ASSERT_EQ(batch->frames.size(), 1U);
    const auto& trace = batch->frames.front();
    EXPECT_EQ(trace.outcome, 1U);
    EXPECT_EQ(trace.sent_fragments, 1U);
    EXPECT_EQ(trace.in_flight_wait_us, 0U);
    EXPECT_EQ(trace.first_in_flight_block.observed_us, 0U);
    const auto injected_us = region == DelayedRegion::kTransportState
        ? trace.transport_state_us : trace.send_call_us;
    RecordProperty("transport_state_us", std::to_string(trace.transport_state_us));
    RecordProperty("send_call_us", std::to_string(trace.send_call_us));
    RecordProperty("in_flight_wait_us", std::to_string(trace.in_flight_wait_us));
    EXPECT_GE(injected_us, 80000U);
}

INSTANTIATE_TEST_SUITE_P(BoundedInjection, SenderCallbackTiming,
    testing::Values(DelayedRegion::kTransportState, DelayedRegion::kSend));
} // namespace
