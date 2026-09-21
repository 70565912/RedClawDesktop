#include "redclaw/diag/host_frame_trace_file.h"
#include "redclaw/net/video_frame_transport.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>

namespace {
using namespace redclaw::net;
using namespace std::chrono_literals;

TEST(HostFrameTrace, DisabledBoundedAndFiniteWithLateCompletion) {
    MediaFrameTraceRecorder recorder;
    EXPECT_FALSE(recorder.active(1));
    EXPECT_FALSE(recorder.arm(100, 121));
    ASSERT_TRUE(recorder.arm(100, 1));
    EXPECT_FALSE(recorder.arm(101, 1));
    MediaFrameTrace frame;
    frame.enabled = true;
    frame.encode_begin_us = 101;
    frame.finish_us = 1100000; // An admitted frame may finish after the arm deadline.
    for (std::size_t i = 0; i <= MediaFrameTraceRecorder::kCapacity; ++i) recorder.record(frame);
    EXPECT_FALSE(recorder.active(102));
    EXPECT_FALSE(recorder.take_completed(1100000));
    auto batch = recorder.take_completed(12000100);
    ASSERT_TRUE(batch);
    EXPECT_EQ(batch->frames.size(), MediaFrameTraceRecorder::kCapacity);
    EXPECT_EQ(batch->overflow, 1U);
    EXPECT_FALSE(recorder.active(12000101));
    ASSERT_TRUE(recorder.arm(13000000, 1));
    recorder.record(frame); // Old in-flight record cannot enter the next window.
    EXPECT_TRUE(recorder.take_completed(25000000)->frames.empty());
}

TEST(HostFrameTrace, SidecarExportsOnlyAfterWindowAndCsvIsNumeric) {
    const auto directory = std::filesystem::temp_directory_path()
        / ("redclaw-frame-trace-test-" + std::to_string(media_trace_now_us()));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{directory};
    const auto log = directory / "runtime.log";
    auto request = log; request += ".frame-trace.request";
    MediaFrameTraceRecorder recorder;
    redclaw::diag::HostFrameTraceFile file(recorder, log);
    { std::ofstream out(request); out << "redclaw.host-frame-trace.v1 1 unexpected"; }
    file.poll(100);
    EXPECT_FALSE(recorder.active(101));
    { std::ofstream out(request); out << "redclaw.host-frame-trace.v1 1"; }
    file.poll(200);
    EXPECT_TRUE(recorder.active(201));
    EXPECT_FALSE(std::filesystem::exists(request));
    MediaFrameTrace frame;
    frame.enabled = true; frame.frame_id = 4; frame.encode_begin_us = 201; frame.outcome = 1;
    recorder.record(frame);
    auto csv = log; csv += ".frame-trace-200.csv";
    file.poll(1000200);
    EXPECT_FALSE(recorder.active(1000200));
    EXPECT_FALSE(std::filesystem::exists(csv));
    file.poll(12000200);
    std::ifstream input(csv);
    ASSERT_TRUE(input.is_open());
    std::string metadata, header, row;
    std::getline(input, metadata); std::getline(input, header); std::getline(input, row);
    EXPECT_NE(metadata.find("overflow=0"), std::string::npos);
    EXPECT_EQ(std::count(header.begin(), header.end(), ','), std::count(row.begin(), row.end(), ','));
    EXPECT_TRUE(std::all_of(row.begin(), row.end(), [](char c) { return c == ',' || (c >= '0' && c <= '9'); }));
}

TEST(HostFrameTrace, PacerTokenAndSendTimesBelongToSameFrame) {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    MediaFrameTraceRecorder recorder;
    const auto start = media_trace_now_us();
    ASSERT_TRUE(recorder.arm(start, 2));
    DesktopMediaSendPacer pacer;
    ASSERT_TRUE(pacer.start([](auto) {
        std::this_thread::sleep_for(2ms);
        return MediaPacerSendResult{.accepted = true};
    }, [] { return MediaPacerTransportState{.open = true}; }, [&](const auto&) {
        // This fixture measures token waits, so acknowledge each packet rather
        // than relying on the former fixed 64 KiB minimum in-flight window.
        pacer.update_budget(1000, 10, 0);
    }, [&](const auto& event) {
        if (event.type == MediaPacerFrameEventType::kSent) {
            { std::lock_guard lock(mutex); done = true; }
            cv.notify_all();
        }
    }, {}, &recorder));
    pacer.update_budget(1000, 10, 0);
    PacedEncodedVideoFrame frame;
    frame.frame_id = 7; frame.rate_revision = 1; frame.codec = 1; frame.width = 320; frame.height = 180;
    frame.target_fps = 30; frame.target_bitrate_kbps = 1000; frame.keyframe = true;
    frame.payload.assign(40000, 0x55);
    frame.trace.enabled = true; frame.trace.encode_begin_us = start + 1;
    ASSERT_TRUE(pacer.submit(std::move(frame)));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 2s, [&] {return done;})); }
    pacer.stop();
    auto batch = recorder.take_completed(start + 13000000);
    ASSERT_TRUE(batch); ASSERT_EQ(batch->frames.size(), 1U);
    const auto& f = batch->frames.front();
    EXPECT_EQ(f.frame_id, 7U); EXPECT_EQ(f.outcome, 1U); EXPECT_TRUE(f.keyframe);
    EXPECT_EQ(f.sent_fragments, f.fragment_count); EXPECT_GT(f.wire_bytes, 40000U);
    EXPECT_GT(f.token_wait_us, 0U); EXPECT_EQ(f.wait_elapsed_us, f.token_wait_us);
    EXPECT_GE(f.send_call_us, 1000U); EXPECT_GE(f.max_send_call_us, 1000U);
    EXPECT_LE(f.wait_overshoot_us, f.wait_elapsed_us);
    EXPECT_LE(f.enqueued_us, f.pacer_begin_us); EXPECT_LE(f.first_send_us, f.last_send_us);
    EXPECT_LE(f.last_send_us, f.finish_us);
}

TEST(HostFrameTrace, BlockingPredicatesAreAttributedWithoutChangingDelivery) {
    for (int reason = 0; reason != 3; ++reason) {
        SCOPED_TRACE(reason);
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        std::atomic<bool> blocked{true};
        MediaFrameTraceRecorder recorder;
        const auto start = media_trace_now_us();
        ASSERT_TRUE(recorder.arm(start, 2));
        DesktopMediaSendPacer pacer;
        ASSERT_TRUE(pacer.start([](auto) {return MediaPacerSendResult{.accepted = true};}, [&] {
            return MediaPacerTransportState{.open = reason != 2 || !blocked.load(),
                .buffered_amount = reason == 1 && blocked.load() ? 65536U : 0U};
        }, [](const auto&) {}, [&](const auto& event) {
            if (event.type == MediaPacerFrameEventType::kSent) {
                { std::lock_guard lock(mutex); done = true; }
                cv.notify_all();
            }
        }, {}, &recorder));
        pacer.update_budget(1000, 10, reason == 0 ? 65536 : 0);
        PacedEncodedVideoFrame frame;
        frame.frame_id = 1; frame.rate_revision = 1; frame.codec = 1; frame.width = 320; frame.height = 180;
        frame.target_fps = 30; frame.target_bitrate_kbps = 1000; frame.keyframe = true;
        frame.payload.assign(100, 0x55); frame.trace.enabled = true; frame.trace.encode_begin_us = start + 1;
        ASSERT_TRUE(pacer.submit(std::move(frame)));
        std::this_thread::sleep_for(30ms);
        blocked.store(false); pacer.update_budget(1000, 10, 0);
        { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 2s, [&] {return done;})); }
        pacer.stop();
        auto batch = recorder.take_completed(start + 13000000);
        ASSERT_TRUE(batch); ASSERT_EQ(batch->frames.size(), 1U);
        const auto& f = batch->frames.front();
        EXPECT_EQ(f.outcome, 1U);
        const auto expected = reason == 0 ? f.in_flight_wait_us : reason == 1 ? f.buffered_wait_us : f.channel_wait_us;
        EXPECT_GT(expected, 0U); EXPECT_EQ(f.wait_elapsed_us, expected);
    }
}
} // namespace
