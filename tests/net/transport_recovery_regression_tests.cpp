#include "redclaw/net/video_frame_transport.h"
#include "redclaw/session/session_module.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
using namespace redclaw::net;
using namespace std::chrono_literals;

PacedEncodedVideoFrame frame(std::uint64_t id, bool keyframe, std::size_t bytes = 100) {
    PacedEncodedVideoFrame f;
    f.frame_id = id;
    f.rate_revision = 1;
    f.codec = 1;
    f.width = 1448;
    f.height = 814;
    f.target_fps = 1;
    f.target_bitrate_kbps = 1769;
    f.keyframe = keyframe;
    f.payload.assign(bytes, 0x55);
    return f;
}

TEST(TransportRecovery, FailureInvalidatesAlreadyQueuedPredictiveFrame) {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false, recovered = false;
    unsigned completed = 0;
    std::atomic<unsigned> sends{0};
    DesktopMediaSendPacer pacer;
    ASSERT_TRUE(pacer.start(
        [&](std::span<const std::uint8_t>) {
            if (++sends == 2) {
                std::unique_lock lock(mutex);
                entered = true;
                cv.notify_all();
                (void)cv.wait_for(lock, 2s, [&] { return released; });
                return MediaPacerSendResult{.accepted = false};
            }
            return MediaPacerSendResult{.accepted = true};
        }, [] { return MediaPacerTransportState{.open = true}; },
        [&](const SentMediaTransportPacket&) { pacer.update_budget(20000, 13, 0); },
        [&](const MediaPacerFrameEvent& e) {
            { std::lock_guard lock(mutex);
              completed += e.type == MediaPacerFrameEventType::kSent;
              recovered |= e.type == MediaPacerFrameEventType::kKeyframeRequired; }
            cv.notify_all();
        }));
    pacer.update_budget(20000, 13, 0);
    ASSERT_TRUE(pacer.submit(frame(1, true)));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return completed == 1; })); }
    ASSERT_TRUE(pacer.submit(frame(2, false)));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return entered; })); }
    ASSERT_TRUE(pacer.submit(frame(3, false)));
    { std::lock_guard lock(mutex); released = true; }
    cv.notify_all();
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return recovered; })); }
    EXPECT_EQ(sends.load(), 2U);
    EXPECT_EQ(pacer.telemetry().dependency_pending_drops, 1U);
    EXPECT_TRUE(pacer.telemetry().keyframe_required);
    EXPECT_FALSE(pacer.submit(frame(4, false)));
    ASSERT_TRUE(pacer.submit(frame(5, true)));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return completed == 2; })); }
    EXPECT_FALSE(pacer.telemetry().keyframe_required);
    pacer.stop();
}

TEST(TransportRecovery, DuplicateFailureRequestsAreCoalescedAndResetStartsNewGeneration) {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned drops = 0;
    std::atomic<unsigned> requests{0};
    DesktopMediaSendPacer pacer;
    ASSERT_TRUE(pacer.start([](std::span<const std::uint8_t>) {
        return MediaPacerSendResult{.accepted = false};
    }, [] { return MediaPacerTransportState{.open = true}; },
    [](const SentMediaTransportPacket&) {}, [&](const MediaPacerFrameEvent& e) {
        if (e.type == MediaPacerFrameEventType::kKeyframeRequired) ++requests;
        if (e.type == MediaPacerFrameEventType::kDropped) {
            { std::lock_guard lock(mutex); ++drops; }
            cv.notify_all();
        }
    }, [&] { cv.notify_all(); }));
    pacer.update_budget(20000, 13, 0);
    for (unsigned i = 1; i <= 3; ++i) {
        ASSERT_TRUE(pacer.submit(frame(i, true)));
        std::unique_lock lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return drops >= i && pacer.telemetry().active_depth == 0; }));
    }
    pacer.reset(); // Drains the active callback before inspecting totals.
    EXPECT_EQ(requests.load(), 1U);
    EXPECT_EQ(pacer.telemetry().suppressed_keyframe_requests, 2U);
    EXPECT_EQ(pacer.telemetry().recovery_generation, 2U);
    EXPECT_TRUE(pacer.telemetry().keyframe_required);
    pacer.stop();
}

TEST(HostStreamScheduling, BlockedAdmissionWaitsForStateAndCannotLoseWakeup) {
    using namespace redclaw::session;
    HostStreamEncodeReadiness ready{true, true, true, true, 100};
    EXPECT_EQ(host_stream_encode_wait(ready, 90), 10ms);
    EXPECT_EQ(host_stream_encode_wait(ready, 100), 0ms);
    ready.capacity_available = false;
    EXPECT_EQ(host_stream_encode_wait(ready, 500), std::chrono::milliseconds::max());
    HostStreamWorkCoordinator work;
    const auto old = work.snapshot().generation;
    work.post(HostStreamWorkReason::kTransportWritable);
    EXPECT_GT(work.wait_for_change(old, std::chrono::milliseconds::max()).generation, old);
    const auto current = work.snapshot().generation;
    auto waiter = std::async(std::launch::async, [&] {
        return work.wait_for_change(current, std::chrono::milliseconds::max());
    });
    EXPECT_EQ(waiter.wait_for(50ms), std::future_status::timeout);
    work.post(HostStreamWorkReason::kStop);
    EXPECT_EQ(waiter.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(waiter.get().contains(HostStreamWorkReason::kStop));
    ready.capacity_available = true;
    ready.channels_ready = false;
    EXPECT_EQ(host_stream_encode_wait(ready, 500), std::chrono::milliseconds::max());
    ready.channels_ready = true;
    ready.source_active = false;
    EXPECT_EQ(host_stream_encode_wait(ready, 500), std::chrono::milliseconds::max());
}
TEST(RecoveryBudget, SlowIdrAndMidFrameRateChangeCompleteWithoutAckBarrier) {
    for (const bool change_rate : {false, true}) {
        std::mutex mutex;
        std::condition_variable cv;
        std::optional<MediaPacerFrameEvent> terminal;
        std::atomic<unsigned> first_packets{0};
        DesktopMediaSendPacer pacer;
        ASSERT_TRUE(pacer.start([](auto) { return MediaPacerSendResult{.accepted = true}; },
            [] { return MediaPacerTransportState{.open = true}; },
            [&](const SentMediaTransportPacket& packet) {
                first_packets += packet.first_packet;
                pacer.update_budget(400, 13, 0);
            }, [&](const MediaPacerFrameEvent& e) {
                if (e.type == MediaPacerFrameEventType::kKeyframeRequired) return;
                { std::lock_guard lock(mutex); terminal = e; }
                cv.notify_all();
            }));
        pacer.update_budget(change_rate ? 1769 : 400, 13, 0);
        ASSERT_TRUE(pacer.submit_frame(frame(1, true, change_rate ? 220000 : 300000)).ready());
        { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 11s, [&] { return terminal.has_value(); })); }
        pacer.stop();
        ASSERT_EQ(terminal->type, MediaPacerFrameEventType::kSent) << terminal->reason;
        EXPECT_EQ(terminal->sent_fragments, terminal->fragment_count);
        EXPECT_EQ(first_packets, 1U);
        EXPECT_LE(terminal->elapsed_us, 10000000U);
        EXPECT_GT(terminal->elapsed_us, 2000000U);
        EXPECT_EQ(pacer.telemetry().deadline_drops, 0U);
    }
}

TEST(RecoveryBudget, InfeasibleAdmissionBacksOffAndWritableDoesNotResetIt) {
    std::mutex mutex;
    std::condition_variable cv;
    DesktopMediaSendPacer pacer;
    std::atomic<unsigned> sent{0}, requested{0};
    ASSERT_TRUE(pacer.start([&](auto) { ++sent; return MediaPacerSendResult{.accepted = true}; },
        [] { return MediaPacerTransportState{.open = true}; }, [](const auto&) {},
        [&](const MediaPacerFrameEvent& e) {
            requested += e.type == MediaPacerFrameEventType::kKeyframeRequired;
        }, [&] { cv.notify_all(); }));
    pacer.update_budget(400, 10, 0);
    ASSERT_TRUE(pacer.submit_frame(frame(1, true, 800000)).ready());
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 2s, [&] {
        return pacer.telemetry().frame_budget_rejections == 1 && pacer.telemetry().active_depth == 0;
    })); }
    const auto blocked = pacer.admission();
    EXPECT_EQ(blocked.state, MediaAdmissionState::kBudgetInfeasible);
    for (int i = 0; i < 100; ++i) pacer.notify_writable();
    EXPECT_EQ(pacer.admission().next_check_ms, blocked.next_check_ms);
    EXPECT_EQ(requested, 0U);
    EXPECT_EQ(sent, 0U);
    pacer.update_budget(1600, 10, 0);
    EXPECT_TRUE(pacer.admission().ready());
    pacer.stop();
}

TEST(RecoveryBudget, ProbeNeedsMediaAckAndCannotTreatHealthyPingAsBandwidth) {
    MediaCongestionController controller;
    MediaCongestionSample sample;
    sample.encoder_target_bitrate_kbps = 400;
    sample.media_channel_open = true;
    sample.rtt_fresh = true;
    sample.smoothed_rtt_ms = 20;
    sample.recovery_budget_blocked = true;
    for (unsigned second = 1; second <= 60; ++second) {
        sample.now_steady_ms = second * 1000;
        sample.rtt_sample_id = second;
        if (second == 2) sample.encoder_target_bitrate_kbps = 2000;
        const auto decision = controller.update(sample);
        EXPECT_LE(decision.pacing_bitrate_kbps, 800U);
        EXPECT_NE(decision.recovery_probe.phase, MediaRecoveryProbePhase::kConfirmed);
        EXPECT_LE(decision.probe_count, second); // No stacked unconfirmed increment.
    }
    controller.reset();
    sample.encoder_target_bitrate_kbps = 400;
    sample.now_steady_ms = 1000; sample.rtt_sample_id = 1;
    const auto probe = controller.update(sample);
    sample.encoder_target_bitrate_kbps = 2000;
    ASSERT_EQ(probe.recovery_probe.phase, MediaRecoveryProbePhase::kProbing);
    sample.now_steady_ms = 1010;
    sample.transport.feedback_fresh = true;
    sample.transport.feedback_sample_id = 1;
    sample.transport.acknowledged_packets = 1;
    EXPECT_EQ(controller.update(sample).recovery_probe.phase, MediaRecoveryProbePhase::kProbing);
    sample.transport.probe_generation = probe.recovery_probe.generation;
    sample.transport.probe_rate_valid = true;
    sample.transport.probe_delivery_bitrate_kbps = probe.pacing_bitrate_kbps;
    sample.transport.latest_acknowledged_sequence = 4;
    sample.demand.probe_end_sequence = 4;
    ++sample.transport.feedback_sample_id;
    EXPECT_EQ(controller.update(sample).recovery_probe.phase, MediaRecoveryProbePhase::kConfirmed);
}

TEST(RecoveryBudget, SlowKeyframeProgressUsesMonotonicAbsoluteCeiling) {
    EncodedVideoFrameReassembler assembler;
    std::vector<std::uint8_t> bytes(100, 1);
    EncodedVideoFragmentView fragment;
    fragment.frame_id = 1; fragment.rate_revision = 1;
    fragment.capture_region_revision = 1; fragment.codec = 1;
    fragment.width = 100; fragment.height = 100; fragment.keyframe = true;
    fragment.fragment_count = 10; fragment.total_payload_bytes = 1000;
    fragment.payload = bytes;
    for (unsigned i = 0; i < 9; ++i) {
        fragment.fragment_index = static_cast<std::uint16_t>(i);
        fragment.transport_sequence = i + 1;
        EXPECT_EQ(assembler.push(fragment, 1000 + i * 1000).status, EncodedVideoReassemblyStatus::kIncomplete);
        EXPECT_TRUE(assembler.keyframe_progressing(1000 + i * 1000, 20));
    }
    EXPECT_FALSE(assembler.expired(10000));
    EXPECT_TRUE(assembler.expired(11500));
    EXPECT_FALSE(assembler.keyframe_progressing(11500, 20));
    EXPECT_FALSE(assembler.keyframe_progressing(500, 20));
}
TEST(RecoveryBudget, UnconfirmedProbeHasHardByteLimitAndCanBeCancelled) {
    DesktopMediaSendPacer pacer;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<std::size_t> bytes{0};
    bool terminal = false;
    MediaRecoveryProbe probe{1, MediaRecoveryProbePhase::kProbing};
    ASSERT_TRUE(pacer.start([&](auto packet) {
        bytes += packet.size();
        return MediaPacerSendResult{.accepted = true};
    }, [] { return MediaPacerTransportState{.open = true}; }, [&](const auto&) {
        pacer.update_budget(500, 10, 0); // Expiry is not a media ACK.
        cv.notify_all();
    }, [&](const auto& e) {
        if (e.type == MediaPacerFrameEventType::kDropped) {
            { std::lock_guard lock(mutex); terminal = true; }
            cv.notify_all();
        }
    }));
    pacer.update_budget(500, 10, 0, &probe);
    ASSERT_TRUE(pacer.submit_frame(frame(1, true, 800000)).ready());
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 3s, [&] { return bytes >= 64U * 1024U; })); }
    EXPECT_EQ(bytes, 64U * 1024U);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(bytes, 64U * 1024U);
    probe.phase = MediaRecoveryProbePhase::kCancelled;
    pacer.update_budget(400, 10, 0, &probe);
    { std::unique_lock lock(mutex); EXPECT_TRUE(cv.wait_for(lock, 1s, [&] { return terminal; })); }
    pacer.stop();
    EXPECT_EQ(bytes, 64U * 1024U);
    EXPECT_GT(pacer.telemetry().next_admission_ms, 0U);
}

TEST(RecoveryBudget, RepeatedRateChangesCannotExtendOriginalHardDeadline) {
    DesktopMediaSendPacer pacer;
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<MediaPacerFrameEvent> terminal;
    std::atomic<unsigned> packets{0};
    ASSERT_TRUE(pacer.start([](auto) { return MediaPacerSendResult{.accepted = true}; },
        [] { return MediaPacerTransportState{.open = true}; }, [&](const auto&) {
            pacer.update_budget((++packets % 2) ? 400 : 410, 10, 0);
        }, [&](const auto& e) {
            if (e.type == MediaPacerFrameEventType::kKeyframeRequired) return;
            { std::lock_guard lock(mutex); terminal = e; }
            cv.notify_all();
        }));
    pacer.update_budget(20000, 10, 0);
    ASSERT_TRUE(pacer.submit_frame(frame(1, true, 800000)).ready());
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 11s, [&] { return terminal.has_value(); })); }
    pacer.stop();
    EXPECT_EQ(terminal->type, MediaPacerFrameEventType::kDropped);
    EXPECT_GE(terminal->elapsed_us, 9900000U);
    EXPECT_LE(terminal->elapsed_us, 10500000U);
    EXPECT_GT(pacer.telemetry().budget_revision, 5U);
}

TEST(RecoveryBudget, GeometryResetCancelsOldCompletionAndPreservesTransportSequence) {
    DesktopMediaSendPacer pacer;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
    unsigned completed = 0;
    std::uint64_t latest_sequence = 0;
    ASSERT_TRUE(pacer.start([](auto) { return MediaPacerSendResult{.accepted = true}; },
        [] { return MediaPacerTransportState{.open = true}; }, [&](const auto& packet) {
            std::unique_lock lock(mutex);
            latest_sequence = packet.transport_sequence;
            if (packet.frame_id == 1) {
                entered = true; cv.notify_all();
                (void)cv.wait_for(lock, 3s, [&] { return released; });
            }
        }, [&](const auto& e) {
            if (e.type == MediaPacerFrameEventType::kSent) {
                { std::lock_guard lock(mutex); ++completed; }
                cv.notify_all();
            }
        }));
    pacer.update_budget(20000, 10, 0);
    ASSERT_TRUE(pacer.submit(frame(1, true)));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return entered; })); }
    auto reset = std::async(std::launch::async, [&] { pacer.reset(false); });
    EXPECT_EQ(reset.wait_for(100ms), std::future_status::timeout);
    { std::lock_guard lock(mutex); released = true; }
    cv.notify_all();
    ASSERT_EQ(reset.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(completed, 0U);
    EXPECT_TRUE(pacer.telemetry().keyframe_required);
    ASSERT_TRUE(pacer.submit(frame(2, true)));
    { std::unique_lock lock(mutex); ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return completed == 1; })); }
    pacer.stop();
    EXPECT_EQ(latest_sequence, 2U);
}

TEST(TransportRecovery, CaptureResetKeepsBothFeedbackEndpointsAdvancing) {
    std::mutex mutex;
    std::condition_variable cv;
    SentMediaTransportPacket sent;
    unsigned completed = 0;
    MediaTransportEstimator estimator;
    MediaTransportFeedbackRecorder receiver;
    DesktopMediaSendPacer pacer;
    ASSERT_TRUE(pacer.start([](auto) { return MediaPacerSendResult{.accepted = true}; },
        [] { return MediaPacerTransportState{.open = true}; },
        [&](const auto& packet) { std::lock_guard lock(mutex); sent = packet; },
        [&](const auto& event) {
            if (event.type == MediaPacerFrameEventType::kSent) {
                { std::lock_guard lock(mutex); ++completed; }
                cv.notify_all();
            }
        }, [&] { cv.notify_all(); }));
    pacer.update_budget(20000, 10, 0);
    for (unsigned index = 1; index <= 5; ++index) {
        if (index == 5) {
            // Only a real connection epoch resets all three sequence owners.
            pacer.reset(true);
            estimator.reset();
            receiver.reset();
            pacer.update_budget(20000, 10, 0);
        } else if (index > 1) {
            pacer.reset(); // Capture availability/region reset in the same connection.
        }
        ASSERT_TRUE(pacer.submit(frame(index, true)));
        SentMediaTransportPacket packet;
        {
            std::unique_lock lock(mutex);
            ASSERT_TRUE(cv.wait_for(lock, 2s, [&] {
                return completed == index && pacer.telemetry().active_depth == 0;
            }));
            packet = sent;
        }
        const auto expected_sequence = index == 5 ? 1U : index;
        ASSERT_EQ(packet.transport_sequence, expected_sequence);
        ASSERT_TRUE(estimator.record_sent(packet));
        const auto arrival_us = packet.steady_send_us + 5000000;
        ASSERT_TRUE(receiver.record(packet.transport_sequence, arrival_us, packet.rate_revision));
        const auto feedback = receiver.take_feedback(arrival_us + 100000);
        ASSERT_TRUE(feedback.has_value());
        ASSERT_TRUE(estimator.apply_feedback(*feedback, packet.steady_send_us + 150000, 1));
        const auto estimate = estimator.snapshot(packet.steady_send_us + 150000, 10);
        EXPECT_EQ(estimate.acknowledged_packets, expected_sequence);
        EXPECT_EQ(estimate.in_flight_bytes, 0U);
        EXPECT_EQ(estimate.ignored_feedback, 0U);
        EXPECT_TRUE(estimate.feedback_fresh);
        pacer.observe_ack_progress(estimate.acknowledged_packets);
        pacer.update_budget(20000, 10, estimate.in_flight_bytes);
    }
    pacer.stop();
}

TEST(RecoveryBudget, FailedPartialFrameMetadataKeepsFirstSendTime) {
    SentVideoFrameMetadataRing ring(256);
    ASSERT_TRUE(ring.record({1, 100, 1, 1, 400, true}));
    ASSERT_TRUE(ring.finish(1, SentVideoFrameState::kDropped, 4));
    const auto metadata = ring.find(1);
    ASSERT_TRUE(metadata.has_value());
    EXPECT_EQ(metadata->steady_send_time_ms, 100U);
    EXPECT_EQ(metadata->sent_fragments, 4U);
    EXPECT_EQ(metadata->state, SentVideoFrameState::kDropped);
}
TEST(HostStreamScheduling, BlockedWorkerHasNoEncodeTicksForFiveSeconds) {
    using namespace redclaw::session;
    HostStreamWorkCoordinator work;
    const auto generation = work.snapshot().generation;
    std::atomic<unsigned> ticks{0};
    std::uint64_t cpu_100ns = 0;
    std::thread worker([&] {
#ifdef _WIN32
        const auto cpu = [] {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return std::uint64_t{0};
            return (std::uint64_t{kernel.dwHighDateTime} << 32) + kernel.dwLowDateTime
                + (std::uint64_t{user.dwHighDateTime} << 32) + user.dwLowDateTime;
        };
        const auto before = cpu();
#endif
        const HostStreamEncodeReadiness blocked{true, true, false, true, 0};
        const auto wait = host_stream_encode_wait(blocked, 1000);
        if (wait == 0ms) ++ticks;
        else (void)work.wait_for_change(generation, wait);
#ifdef _WIN32
        cpu_100ns = cpu() - before;
#endif
    });
    std::this_thread::sleep_for(5s);
    work.post(HostStreamWorkReason::kStop);
    worker.join();
    EXPECT_EQ(ticks, 0U);
#ifdef _WIN32
    RecordProperty("blocked_worker_cpu_ms", static_cast<int>(cpu_100ns / 10000));
    EXPECT_LT(cpu_100ns / 10000, 250U);
#endif
}
} // namespace
