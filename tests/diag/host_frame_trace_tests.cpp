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
    EXPECT_LE(sizeof(MediaFrameTrace) * MediaFrameTraceRecorder::kCapacity, 4U * 1024U * 1024U);
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
    frame.first_in_flight_block.oldest_sent_us = 123;
    frame.final_admission.estimate_us = 234;
    recorder.record(frame);
    auto csv = log; csv += ".frame-trace-200.csv";
    file.poll(1000200);
    EXPECT_FALSE(recorder.active(1000200));
    EXPECT_FALSE(std::filesystem::exists(csv));
    file.poll(12000200);
    auto pending = csv; pending += ".tmp";
    EXPECT_FALSE(std::filesystem::exists(pending));
    std::ifstream input(csv);
    ASSERT_TRUE(input.is_open());
    std::string metadata, header, row;
    std::getline(input, metadata); std::getline(input, header); std::getline(input, row);
    EXPECT_NE(metadata.find("overflow=0"), std::string::npos);
    EXPECT_TRUE(metadata.starts_with("# redclaw.host-frame-trace.v2 "));
    EXPECT_NE(header.find("block_oldest_sent_us"), std::string::npos);
    EXPECT_NE(header.find("final_estimate_us"), std::string::npos);
    EXPECT_EQ(std::count(header.begin(), header.end(), ','), std::count(row.begin(), row.end(), ','));
    EXPECT_TRUE(std::all_of(row.begin(), row.end(), [](char c) { return c == ',' || (c >= '0' && c <= '9'); }));
    std::istringstream names(header), values(row);
    std::string name, value;
    while (std::getline(names, name, ',') && std::getline(values, value, ',')) {
        if (name == "block_oldest_sent_us") EXPECT_EQ(value, "123");
        if (name == "final_estimate_us") EXPECT_EQ(value, "234");
    }
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
    EXPECT_EQ(f.wait_count, f.timeout_wakes + f.notified_wakes);
    EXPECT_GT(f.wait_count, 0U);
    EXPECT_EQ(f.first_in_flight_block.observed_us, 0U);
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

TEST(HostFrameTrace, AdmissionProvenanceSeparatesAckExpiryAndMissingRefresh) {
    for (int mode = 0; mode != 4; ++mode) {
        SCOPED_TRACE(mode); // 0 ACK, 1 external expiry, 2 stale copy, 3 scheduled expiry.
        std::mutex mutex;
        std::condition_variable cv;
        std::optional<MediaPacerFrameEvent> terminal;
        std::atomic<bool> queried{false};
        std::atomic<unsigned> refreshes{0};
        MediaFrameTraceRecorder recorder;
        MediaTransportEstimator estimator;
        const auto start = media_trace_now_us();
        ASSERT_TRUE(recorder.arm(start, 3));
        // Already nearly expired when this frame reaches the sender. Expiry
        // alone cannot release the pacer's copy of the last estimator snapshot.
        ASSERT_TRUE(estimator.record_sent({1, 1, start - 740000, 1, 16000}));
        auto estimate = estimator.snapshot(start, 9);
        DesktopMediaSendPacer pacer;
        ASSERT_TRUE(pacer.start([](auto) { return MediaPacerSendResult{.accepted = true}; }, [&] {
            queried.store(true); cv.notify_all();
            return MediaPacerTransportState{.open = true};
        }, [](const auto&) {}, [&](const MediaPacerFrameEvent& event) {
            if (event.type == MediaPacerFrameEventType::kKeyframeRequired) return;
            { std::lock_guard lock(mutex); terminal = event; }
            cv.notify_all();
        }, {}, &recorder, mode == 3 ? MediaPacerInFlightRefreshCallback{
            [&](std::uint64_t now, std::uint32_t rtt) {
                ++refreshes;
                return estimator.snapshot(now, rtt);
            }} : MediaPacerInFlightRefreshCallback{}));
        MediaCongestionDecision policy;
        policy.decision_revision = 2;
        policy.pacing_bitrate_kbps = 8966;
        policy.in_flight_limit_bytes = 16384;
        policy.feedback_horizon_us = 100000;
        pacer.update_policy(policy, 9, estimate.in_flight_bytes, &estimate);
        PacedEncodedVideoFrame frame;
        frame.frame_id = 2; frame.rate_revision = 1; frame.codec = 1;
        frame.width = 1778; frame.height = 1000; frame.target_fps = 30;
        frame.target_bitrate_kbps = 4267; frame.keyframe = true;
        frame.payload.assign(16063 - 80, 0x55);
        frame.trace.enabled = true; frame.trace.encode_begin_us = start + 1;
        ASSERT_TRUE(pacer.submit(std::move(frame)));
        { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 1s, [&] { return queried.load(); })); }
        std::this_thread::sleep_for(30ms);
        if (mode < 2) {
            const auto now = media_trace_now_us();
            if (mode == 0) ASSERT_TRUE(estimator.apply_feedback({1, 1, {{1, 9000000}}}, now, 1));
            estimate = estimator.snapshot(now, 9);
            ASSERT_EQ(estimate.in_flight_bytes, 0U);
            auto stale_policy = policy;
            stale_policy.decision_revision = 1;
            pacer.update_policy(stale_policy, 9, 99999, &estimate);
            pacer.update_policy(policy, 9, estimate.in_flight_bytes, &estimate);
        }
        { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 3s, [&] { return terminal.has_value(); })); }
        pacer.stop(); // Drains trace completion before reading it.
        auto batch = recorder.take_completed(start + 14000000);
        ASSERT_TRUE(batch); ASSERT_EQ(batch->frames.size(), 1U);
        const auto& f = batch->frames.front();
        EXPECT_EQ(f.blocked_wire_bytes, 16063U);
        RecordProperty("admission_mode_" + std::to_string(mode) + "_elapsed_us",
            std::to_string(f.finish_us - f.pacer_begin_us));
        EXPECT_GT(f.first_in_flight_block.observed_us, 0U);
        EXPECT_EQ(f.first_in_flight_block.estimated_bytes, 16000U);
        EXPECT_GT(f.first_in_flight_block.in_flight_bytes + f.blocked_wire_bytes,
            f.first_in_flight_block.limit_bytes);
        EXPECT_EQ(f.first_in_flight_block.oldest_sent_us, start - 740000);
        EXPECT_EQ(f.wait_count, f.timeout_wakes + f.notified_wakes);
        EXPECT_GT(f.in_flight_wait_us, 0U);
        if (mode == 2) {
            EXPECT_EQ(f.outcome, 2U);
            EXPECT_EQ(terminal->reason, "pacer_in_flight_deadline");
            EXPECT_EQ(f.sent_fragments, 0U);
            EXPECT_EQ(f.final_admission.estimate_us, start);
            EXPECT_EQ(f.final_admission.expired_packets, 0U);
            EXPECT_GT(f.timeout_wakes, 0U);
            const auto refreshed = estimator.snapshot(media_trace_now_us(), 9);
            EXPECT_EQ(refreshed.expired_in_flight_packets, 1U);
            EXPECT_EQ(refreshed.in_flight_bytes, 0U);
        } else if (mode < 2) {
            EXPECT_EQ(f.outcome, 1U);
            EXPECT_EQ(f.sent_fragments, 1U);
            EXPECT_EQ(f.final_admission.expired_packets, mode == 1 ? 1U : 0U);
            EXPECT_EQ(f.final_admission.acknowledged_sequence, mode == 0 ? 1U : 0U);
            EXPECT_EQ(f.final_admission.rejected_policy_updates, 1U);
            EXPECT_EQ(f.final_admission.policy_revision, 2U);
            EXPECT_EQ(f.final_admission.budget_updates, 2U);
            EXPECT_GT(f.notified_wakes, 0U);
        } else {
            EXPECT_EQ(f.outcome, 1U);
            EXPECT_EQ(f.sent_fragments, 1U);
            EXPECT_EQ(f.final_admission.expired_packets, 1U);
            EXPECT_EQ(f.final_admission.acknowledged_sequence, 0U);
            EXPECT_EQ(f.final_admission.policy_revision, 2U);
            EXPECT_EQ(pacer.telemetry().deadline_drops, 0U);
            EXPECT_LT(f.finish_us - f.pacer_begin_us, 500000U);
            EXPECT_GE(refreshes.load(), 1U);
            EXPECT_LE(refreshes.load(), 2U); // No polling loop or heartbeat.
        }
    }
}

TEST(HostFrameTrace, ExpiryRefreshAdvancesAcrossMultipleRetainedPackets) {
    MediaTransportEstimator estimator;
    const auto start = media_trace_now_us();
    ASSERT_TRUE(estimator.record_sent({1, 1, start - 720000, 1, 8000}));
    ASSERT_TRUE(estimator.record_sent({2, 1, start - 600000, 1, 8000}));
    auto estimate = estimator.snapshot(start, 9);
    MediaFrameTraceRecorder recorder;
    ASSERT_TRUE(recorder.arm(start, 3));
    std::atomic<unsigned> refreshes{0};
    std::promise<MediaPacerFrameEvent> terminal;
    auto done = terminal.get_future();
    DesktopMediaSendPacer pacer;
    ASSERT_TRUE(pacer.start([](auto) { return MediaPacerSendResult{.accepted = true}; },
        [] { return MediaPacerTransportState{.open = true}; }, [](const auto&) {},
        [&](const auto& event) {
            if (event.type != MediaPacerFrameEventType::kKeyframeRequired) terminal.set_value(event);
        }, {}, &recorder, [&](auto now, auto rtt) {
            ++refreshes;
            return estimator.snapshot(now, rtt);
        }));
    MediaCongestionDecision policy;
    policy.decision_revision = 2; policy.pacing_bitrate_kbps = 8966;
    policy.in_flight_limit_bytes = 16384; policy.feedback_horizon_us = 100000;
    pacer.update_policy(policy, 9, estimate.in_flight_bytes, &estimate);
    PacedEncodedVideoFrame frame;
    frame.frame_id = 3; frame.rate_revision = 1; frame.codec = 1;
    frame.width = 1778; frame.height = 1000; frame.target_fps = 30;
    frame.target_bitrate_kbps = 4267; frame.keyframe = true;
    frame.payload.assign(16063 - 80, 0x55); frame.trace.enabled = true;
    frame.trace.encode_begin_us = start + 1;
    ASSERT_TRUE(pacer.submit(std::move(frame)));
    ASSERT_EQ(done.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(done.get().type, MediaPacerFrameEventType::kSent);
    pacer.stop();
    auto batch = recorder.take_completed(start + 14000000);
    ASSERT_TRUE(batch); ASSERT_EQ(batch->frames.size(), 1U);
    const auto& trace = batch->frames.front();
    EXPECT_EQ(trace.outcome, 1U);
    EXPECT_EQ(trace.final_admission.expired_packets, 2U);
    EXPECT_EQ(trace.final_admission.acknowledged_sequence, 0U);
    EXPECT_LT(trace.finish_us - trace.pacer_begin_us, 500000U);
    EXPECT_LE(refreshes.load(), 3U);
    EXPECT_EQ(estimator.snapshot(media_trace_now_us(), 9).lost_packets, 0U);
}

TEST(HostFrameTrace, RefreshCannotOverwriteConcurrentBudgetOrReset) {
    for (bool reset : {false, true}) {
        SCOPED_TRACE(reset);
        std::atomic<unsigned> refreshes{0}, sent{0};
        std::promise<void> completed;
        auto done = completed.get_future();
        DesktopMediaSendPacer pacer;
        ASSERT_TRUE(pacer.start([&](auto) { ++sent; return MediaPacerSendResult{.accepted = true}; },
            [] { return MediaPacerTransportState{.open = true}; }, [](const auto&) {},
            [&](const auto& event) {
                if (event.type == MediaPacerFrameEventType::kSent) completed.set_value();
            }, {}, nullptr, [&](auto now, auto) {
                ++refreshes;
                // These calls also establish that the estimator callback does not
                // run under the pacer mutex. Its stale result must be discarded.
                if (reset) pacer.reset(true);
                pacer.update_budget(8966, 9, 0);
                return MediaTransportEstimate{.in_flight_bytes = 999999,
                    .sampled_host_us = now, .oldest_in_flight_sent_us = now};
            }));
        pacer.update_budget(8966, 9, 999999);
        auto submit = [&](std::uint64_t id) {
            PacedEncodedVideoFrame frame;
            frame.frame_id = id; frame.rate_revision = 1; frame.codec = 1;
            frame.width = 16; frame.height = 16; frame.target_fps = 30;
            frame.target_bitrate_kbps = 4267; frame.keyframe = true;
            frame.payload.assign(100, 0x55);
            return pacer.submit(std::move(frame));
        };
        ASSERT_TRUE(submit(1));
        if (reset) {
            const auto limit = std::chrono::steady_clock::now() + 1s;
            while ((refreshes.load() == 0 || pacer.telemetry().active_depth != 0)
                && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(1ms);
            ASSERT_EQ(refreshes.load(), 1U);
            EXPECT_EQ(sent.load(), 0U);
            ASSERT_TRUE(submit(2));
        }
        ASSERT_EQ(done.wait_for(2s), std::future_status::ready);
        pacer.stop();
        EXPECT_EQ(refreshes.load(), 1U);
        EXPECT_EQ(sent.load(), 1U);
        EXPECT_EQ(pacer.telemetry().deadline_drops, 0U);
    }
}
} // namespace
