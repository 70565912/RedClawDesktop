#include "../../src/net/src/media_pacer_wait.h"
#include "redclaw/net/video_frame_transport.h"

#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <thread>

namespace {
using namespace redclaw::net;
using namespace std::chrono_literals;

class PacerWait : public testing::TestWithParam<MediaPacerWait::Mode> {};

TEST_P(PacerWait, StaleNotificationCannotGrantCreditOrSendEarly) {
    MediaPacerWait wait(GetParam());
    std::mutex mutex;
    std::unique_lock lock(mutex);
    wait.notify();
    const auto begin = MediaPacerWait::Clock::now();
    EXPECT_FALSE(wait.wait_for(lock, 20ms, [] { return false; }));
    EXPECT_GE(MediaPacerWait::Clock::now() - begin, 20ms);
    // A predicate made true before the waiter starts must not be lost.
    EXPECT_TRUE(wait.wait_for(lock, 5s, [] { return true; }));
}

TEST_P(PacerWait, PredicateChangeAtUnlockCancelsLongWaitRepeatedly) {
    MediaPacerWait wait(GetParam());
    std::mutex mutex;
    for (unsigned generation = 0; generation < 16; ++generation) {
        bool cancelled = false;
        std::promise<void> locked;
        auto result = std::async(std::launch::async, [&] {
            std::unique_lock lock(mutex);
            locked.set_value();
            return wait.wait_for(lock, 2s, [&] { return cancelled; });
        });
        locked.get_future().wait();
        { std::lock_guard lock(mutex); cancelled = true; }
        wait.notify();
        EXPECT_EQ(result.wait_for(500ms), std::future_status::ready);
        EXPECT_TRUE(result.get());
    }
}

TEST_P(PacerWait, UnrelatedWakeStormDoesNotExtendOriginalDeadline) {
    MediaPacerWait wait(GetParam());
    std::mutex mutex;
    std::jthread producer([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            { std::lock_guard lock(mutex); wait.notify(); }
            std::this_thread::sleep_for(1ms);
        }
    });
    std::unique_lock lock(mutex);
    const auto begin = MediaPacerWait::Clock::now();
    EXPECT_FALSE(wait.wait_for(lock, 50ms, [] { return false; }));
    const auto elapsed = MediaPacerWait::Clock::now() - begin;
    lock.unlock();
    producer.request_stop();
    EXPECT_GE(elapsed, 50ms);
    EXPECT_LT(elapsed, 500ms);
}

INSTANTIATE_TEST_SUITE_P(Backends, PacerWait, testing::Values(
    MediaPacerWait::Mode::kAutomatic, MediaPacerWait::Mode::kConditionVariable));

PacedEncodedVideoFrame small_frame(std::uint64_t id) {
    PacedEncodedVideoFrame frame;
    frame.frame_id = id;
    frame.rate_revision = frame.codec = 1;
    frame.width = 640;
    frame.height = 360;
    frame.target_fps = 1;
    frame.target_bitrate_kbps = 1000;
    frame.keyframe = true;
    frame.payload.assign(100, 0x55);
    return frame;
}

TEST(PacerWakeLifecycle, ResetAndStopCancelLongAdmissionWait) {
    std::mutex mutex;
    std::condition_variable changed;
    DesktopMediaSendPacer pacer;
    ASSERT_TRUE(pacer.start([](auto) { return MediaPacerSendResult{.accepted = true}; },
        [] { return MediaPacerTransportState{.open = false}; }, [](const auto&) {},
        {}, [&] { changed.notify_all(); }));
    pacer.update_budget(12000, 10, 0);
    for (unsigned index = 1; index <= 3; ++index) {
        ASSERT_TRUE(pacer.submit(small_frame(index)));
        { std::unique_lock lock(mutex);
          ASSERT_TRUE(changed.wait_for(lock, 1s, [&] { return pacer.telemetry().active_depth == 1; })); }
        std::this_thread::sleep_for(20ms);
        auto cancel = std::async(std::launch::async, [&] {
            if (index == 3) pacer.stop();
            else pacer.reset(false);
        });
        EXPECT_EQ(cancel.wait_for(500ms), std::future_status::ready);
        cancel.get();
        EXPECT_EQ(pacer.telemetry().active_depth, 0U);
        EXPECT_EQ(pacer.telemetry().frames_sent, 0U);
    }
}

TEST(PacerWakeLifecycle, CadenceSurvivesNotificationsAndResetAllowsNewGeneration) {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<unsigned> sent{0};
    DesktopMediaSendPacer pacer;
    ASSERT_TRUE(pacer.start([](auto) { return MediaPacerSendResult{.accepted = true}; },
        [] { return MediaPacerTransportState{.open = true}; }, [](const auto&) {},
        [&](const auto& event) {
            if (event.type == MediaPacerFrameEventType::kSent) ++sent;
            changed.notify_all();
        }, [&] { changed.notify_all(); }));
    pacer.update_budget(12000, 10, 0);
    ASSERT_TRUE(pacer.submit(small_frame(1)));
    { std::unique_lock lock(mutex);
      ASSERT_TRUE(changed.wait_for(lock, 1s, [&] { return sent == 1 && pacer.can_accept_frame(); })); }
    ASSERT_TRUE(pacer.submit(small_frame(2)));
    for (unsigned i = 0; i < 10; ++i) {
        pacer.notify_writable();
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(sent, 1U); // Wakes alone must not bypass the one-second frame clock.
    pacer.reset(false);
    ASSERT_TRUE(pacer.submit(small_frame(3)));
    { std::unique_lock lock(mutex);
      EXPECT_TRUE(changed.wait_for(lock, 500ms, [&] { return sent == 2 && pacer.can_accept_frame(); })); }
    ASSERT_TRUE(pacer.submit(small_frame(4)));
    std::this_thread::sleep_for(20ms);
    auto stop = std::async(std::launch::async, [&] { pacer.stop(); });
    EXPECT_EQ(stop.wait_for(500ms), std::future_status::ready);
    stop.get();
    EXPECT_EQ(sent, 2U);
}
} // namespace
