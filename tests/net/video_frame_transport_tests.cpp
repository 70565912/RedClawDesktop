#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "redclaw/net/video_frame_transport.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }
    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_fragment_round_trip_reuses_packet_capacity() {
    std::vector<std::uint8_t> payload(40000);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>(index & 0xffu);
    }

    redclaw::net::EncodedVideoFrameView frame;
    frame.frame_id = 42;
    frame.rate_revision = 7;
    frame.capture_region_revision = 13;
    frame.codec = 1;
    frame.width = 1920;
    frame.height = 1080;
    frame.content_rect_x = 240;
    frame.content_rect_y = 0;
    frame.content_rect_width = 1440;
    frame.content_rect_height = 1080;
    frame.timestamp_ms = 123456;
    frame.keyframe = true;
    frame.payload = payload;

    redclaw::net::EncodedVideoFragmentPlan plan;
    std::string error;
    if (!redclaw::net::build_encoded_video_fragment_plan(frame, 16 * 1024, &plan, &error)) {
        return expect_true(false, "fragment plan should succeed: " + error);
    }

    bool ok = true;
    ok = expect_true(plan.fragment_count == 3, "40000-byte frame should require three fragments") && ok;
    ok = expect_true(plan.wire_bytes == payload.size() + 3 * 80, "wire size should include one header per fragment") && ok;

    std::vector<std::uint8_t> packet;
    packet.reserve(16 * 1024);
    const std::size_t reserved_capacity = packet.capacity();
    std::vector<std::uint8_t> reassembled;
    reassembled.reserve(payload.size());

    for (std::uint16_t index = 0; index < plan.fragment_count; ++index) {
        if (!redclaw::net::serialize_encoded_video_fragment(
                frame, plan, index, 100 + index, &packet, &error)) {
            return expect_true(false, "fragment serialization should succeed: " + error);
        }

        redclaw::net::EncodedVideoFragmentView fragment;
        if (!redclaw::net::parse_encoded_video_fragment(packet, &fragment, &error)) {
            return expect_true(false, "fragment parse should succeed: " + error);
        }
        ok = expect_true(fragment.frame_id == frame.frame_id, "frame id should round-trip") && ok;
        ok = expect_true(fragment.rate_revision == frame.rate_revision, "rate revision should round-trip") && ok;
        ok = expect_true(fragment.capture_region_revision == 13, "capture region revision should round-trip") && ok;
        ok = expect_true(fragment.content_rect_x == 240 && fragment.content_rect_width == 1440,
                         "effective content rect should round-trip") && ok;
        ok = expect_true(fragment.transport_sequence == 100 + index, "transport sequence should round-trip") && ok;
        ok = expect_true(fragment.fragment_index == index, "fragment index should round-trip") && ok;
        ok = expect_true(fragment.fragment_count == plan.fragment_count, "fragment count should round-trip") && ok;
        reassembled.insert(reassembled.end(), fragment.payload.begin(), fragment.payload.end());
    }

    ok = expect_true(packet.capacity() == reserved_capacity, "packet vector capacity should be reused") && ok;
    ok = expect_true(reassembled == payload, "reassembled payload should match source bytes") && ok;
    return ok;
}

bool test_rejects_invalid_inputs() {
    redclaw::net::EncodedVideoFrameView frame;
    redclaw::net::EncodedVideoFragmentPlan plan;
    std::string error;

    bool ok = true;
    ok = expect_true(
        !redclaw::net::build_encoded_video_fragment_plan(frame, 16 * 1024, &plan, &error),
        "empty frame metadata should be rejected") && ok;

    const std::vector<std::uint8_t> truncated(39, 0);
    redclaw::net::EncodedVideoFragmentView fragment;
    ok = expect_true(
        !redclaw::net::parse_encoded_video_fragment(truncated, &fragment, &error),
        "truncated fragment should be rejected") && ok;

    std::vector<std::uint8_t> payload(100, 0x44);
    frame = {
        .frame_id = 1,
        .rate_revision = 1,
        .codec = 1,
        .width = 640,
        .height = 480,
        .timestamp_ms = 1,
        .keyframe = true,
        .payload = payload,
    };
    std::vector<std::uint8_t> packet;
    ok = expect_true(
        redclaw::net::build_encoded_video_fragment_plan(
            frame, 16 * 1024, &plan, &error)
            && redclaw::net::serialize_encoded_video_fragment(
                frame, plan, 0, 1, &packet, &error),
        "a valid v4 packet should serialize") && ok;
    packet[4] = 3;
    ok = expect_true(
        !redclaw::net::parse_encoded_video_fragment(packet, &fragment, &error),
        "the synchronized v4 transport must reject old v3 fragments") && ok;
    return ok;
}

bool test_navigation_thumbnail_is_bounded_and_latest_ready() {
    const std::vector<std::uint8_t> jpeg{0xff, 0xd8, 0xff, 0xd9};
    redclaw::net::NavigationThumbnailView view;
    view.catalog_revision = 4;
    view.thumbnail_revision = 17;
    view.width = 320;
    view.height = 180;
    view.display_id = "luid-2-output-1";
    view.jpeg = jpeg;

    std::vector<std::uint8_t> packet;
    std::string error;
    bool ok = expect_true(
        redclaw::net::serialize_navigation_thumbnail(view, &packet, &error),
        "a bounded navigation thumbnail should serialize: " + error);
    redclaw::net::NavigationThumbnail parsed;
    ok = expect_true(
        redclaw::net::parse_navigation_thumbnail(packet, &parsed, &error),
        "a bounded navigation thumbnail should parse: " + error) && ok;
    ok = expect_true(parsed.catalog_revision == 4 && parsed.thumbnail_revision == 17,
                     "navigation revisions should round-trip") && ok;
    ok = expect_true(parsed.width == 320 && parsed.height == 180,
                     "navigation dimensions should round-trip") && ok;
    ok = expect_true(parsed.display_id == view.display_id && parsed.jpeg == jpeg,
                     "navigation display and JPEG should round-trip") && ok;

    view.width = 321;
    ok = expect_true(
        !redclaw::net::serialize_navigation_thumbnail(view, &packet, &error),
        "a thumbnail wider than 320 pixels must be rejected") && ok;
    view.width = 320;
    std::vector<std::uint8_t> oversized(
        redclaw::net::kMaxNavigationThumbnailJpegBytes + 1U, 0x44);
    view.jpeg = oversized;
    ok = expect_true(
        !redclaw::net::serialize_navigation_thumbnail(view, &packet, &error),
        "an oversized navigation JPEG must be rejected") && ok;
    return ok;
}

bool test_transport_feedback_recorder_batches_and_resets() {
    redclaw::net::MediaTransportFeedbackRecorder recorder;
    bool ok = true;
    for (std::uint64_t sequence = 1; sequence <= 31; ++sequence) {
        ok = expect_true(
            recorder.record(sequence, 1000000 + sequence * 1000, 4),
            "strictly increasing arrivals should be recorded") && ok;
    }
    ok = expect_true(
        !recorder.feedback_due(1099999),
        "sparse feedback should wait for the 100 ms timer") && ok;
    ok = expect_true(
        recorder.feedback_due(1101000),
        "sparse feedback should flush after 100 ms") && ok;
    const auto timed = recorder.take_feedback(1101000);
    ok = expect_true(
        timed.has_value() && timed->feedback_id == 1
            && timed->arrivals.size() == 31,
        "timer feedback should include every pending arrival") && ok;

    for (std::uint64_t sequence = 32; sequence <= 63; ++sequence) {
        ok = expect_true(
            recorder.record(sequence, 1200000 + sequence * 1000, 4),
            "the next arrival window should remain monotonic") && ok;
    }
    const auto count_triggered = recorder.take_feedback(1232000);
    ok = expect_true(
        count_triggered.has_value() && count_triggered->feedback_id == 2
            && count_triggered->arrivals.size() == 32,
        "32 arrivals should trigger feedback before the timer") && ok;
    ok = expect_true(
        !recorder.record(63, 1300000, 4),
        "duplicate transport sequences should be rejected") && ok;

    recorder.reset();
    ok = expect_true(
        recorder.pending_arrivals() == 0
            && recorder.record(1, 2000000, 9),
        "epoch reset should clear sequence and pending feedback state") && ok;
    return ok;
}

bool test_transport_estimator_tracks_rate_gap_queue_and_replay() {
    redclaw::net::MediaTransportEstimator estimator;
    constexpr std::uint64_t kSendBaseUs = 1000000;
    constexpr std::uint64_t kArrivalBaseUs = 1050000;
    for (std::uint64_t sequence = 1; sequence <= 20; ++sequence) {
        if (!estimator.record_sent({
                .transport_sequence = sequence,
                .frame_id = sequence,
                .steady_send_us = kSendBaseUs + sequence * 10000,
                .rate_revision = 7,
                .wire_bytes = 25000,
                .keyframe = sequence == 1,
            })) {
            return expect_true(false, "sent transport metadata should be accepted");
        }
    }

    redclaw::net::MediaTransportFeedbackBatch feedback;
    feedback.feedback_id = 1;
    feedback.observed_rate_revision = 7;
    for (std::uint64_t sequence = 1; sequence <= 20; ++sequence) {
        if (sequence == 8) {
            continue;
        }
        const std::uint64_t added_queue_us = sequence >= 10 ? 200000 : 0;
        feedback.arrivals.push_back({
            sequence,
            kArrivalBaseUs + sequence * 10000 + added_queue_us,
        });
    }

    bool ok = true;
    ok = expect_true(
        estimator.apply_feedback(feedback, 2000000, 7),
        "revision-matched feedback should update the transport estimate") && ok;
    const auto estimate = estimator.snapshot(2050000, 80);
    ok = expect_true(
        estimate.feedback_fresh && estimate.rate_revision == 7
            && estimate.acknowledged_bitrate_kbps > 0,
        "acknowledged delivery should produce a fresh nonzero bitrate") && ok;
    ok = expect_true(
        estimate.loss_per_mille >= 40 && estimate.lost_packets == 1,
        "a transport sequence gap should be counted as loss") && ok;
    ok = expect_true(
        estimate.queue_delay_ms >= 100,
        "sustained additional arrival delay should raise the relative queue estimate") && ok;
    ok = expect_true(
        !estimator.apply_feedback(feedback, 2100000, 7),
        "replayed feedback ids should be ignored") && ok;
    ok = expect_true(
        estimator.snapshot(2100000, 80).ignored_feedback == 1,
        "ignored feedback should be observable without changing the rate") && ok;

    estimator.reset();
    const auto reset = estimator.snapshot(2200000, 80);
    ok = expect_true(
        reset.in_flight_bytes == 0 && reset.acknowledged_packets == 0
            && reset.ignored_feedback == 0,
        "epoch reset should clear sent metadata and feedback history") && ok;
    return ok;
}

bool test_stale_revision_feedback_retires_only_old_in_flight_packets() {
    redclaw::net::MediaTransportEstimator estimator;
    bool ok = true;
    for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
        ok = expect_true(
            estimator.record_sent({
                .transport_sequence = sequence,
                .frame_id = sequence,
                .steady_send_us = 1000000 + sequence * 10000,
                .rate_revision = sequence <= 3 ? 7ULL : 8ULL,
                .wire_bytes = 1000,
                .keyframe = sequence == 1,
            }),
            "sent metadata for two revisions should be accepted") && ok;
    }

    redclaw::net::MediaTransportFeedbackBatch stale_feedback;
    stale_feedback.feedback_id = 1;
    stale_feedback.observed_rate_revision = 7;
    stale_feedback.arrivals = {
        {1, 1100000},
        {2, 1110000},
        {3, 1120000},
        // An untrusted old-revision report must not retire or advance past a
        // packet that the Host knows belongs to the newer revision.
        {4, 1130000},
    };
    ok = expect_true(
        !estimator.apply_feedback(stale_feedback, 1200000, 8),
        "old-revision delivery must not refresh the current rate estimate") && ok;
    const auto after_stale = estimator.snapshot(1200000, 80);
    ok = expect_true(
        after_stale.in_flight_bytes == 1000
            && after_stale.acknowledged_packets == 3
            && after_stale.ignored_feedback == 1
            && after_stale.rate_revision == 0
            && !after_stale.feedback_fresh,
        "stale feedback should retire old payload-free metadata without consuming the newer revision") && ok;

    redclaw::net::MediaTransportFeedbackBatch current_feedback;
    current_feedback.feedback_id = 2;
    current_feedback.observed_rate_revision = 8;
    current_feedback.arrivals = {{4, 1210000}};
    ok = expect_true(
        estimator.apply_feedback(current_feedback, 1220000, 8),
        "current-revision feedback should remain applicable after old metadata retirement") && ok;
    const auto current = estimator.snapshot(1230000, 80);
    ok = expect_true(
        current.in_flight_bytes == 0
            && current.acknowledged_packets == 4
            && current.rate_revision == 8
            && current.feedback_fresh,
        "the current acknowledgement should drain the remaining in-flight packet") && ok;
    return ok;
}

bool test_congestion_controller_avoids_double_loss_backoff_and_probes() {
    redclaw::net::MediaCongestionController controller;
    redclaw::net::MediaCongestionSample sample;
    sample.encoder_target_bitrate_kbps = 20000;
    sample.smoothed_rtt_ms = 180;
    sample.rtt_fresh = sample.media_channel_open = true;
    sample.transport.feedback_fresh = sample.transport.delivery_rate_valid = true;
    sample.transport.application_limited = false;
    sample.transport.delivery_bitrate_kbps = 20000;
    sample.transport.feedback_interval_us = 100000;
    sample.transport.feedback_sample_id = 1;
    sample.demand.target_fps = 30;
    sample.now_steady_ms = 1000;
    auto decision = controller.update(sample);
    bool ok = expect_true(decision.pacing_bitrate_kbps == 20000 && !decision.backoff,
        "high base RTT without queue growth is not congestion");
    sample.now_steady_ms = 1100;
    ++sample.transport.feedback_sample_id;
    sample.encoder_target_bitrate_kbps = 400;
    decision = controller.update(sample);
    ok = expect_true(decision.pacing_bitrate_kbps == 20000,
        "encoder average bitrate is not a pacing ceiling") && ok;
    sample.transport.loss_per_mille = 10;
    ++sample.transport.feedback_sample_id;
    decision = controller.update(sample);
    ok = expect_true(decision.isolated_loss && !decision.backoff,
        "isolated loss does not manufacture a capacity drop") && ok;
    sample.now_steady_ms = 1500;
    ++sample.transport.feedback_sample_id;
    sample.transport.queue_delay_ms = 200;
    sample.transport.delivery_bitrate_kbps = 10000;
    decision = controller.update(sample);
    const auto drained = decision.pacing_bitrate_kbps;
    ok = expect_true(decision.backoff && drained < 10000,
        "measured congestion drains below the demonstrated delivery rate") && ok;
    ok = expect_true(!controller.update(sample).backoff,
        "the same feedback cannot repeatedly back off") && ok;
    sample.now_steady_ms = 3000;
    ++sample.transport.feedback_sample_id;
    sample.transport.queue_delay_ms = sample.transport.loss_per_mille = 0;
    sample.transport.delivery_bitrate_kbps = drained;
    sample.demand.pending_bytes = 512 * 1024;
    sample.demand.token_limited = true;
    decision = controller.update(sample);
    ok = expect_true(decision.probe && decision.pacing_bitrate_kbps > drained,
        "fresh delivery and queued demand can explore headroom independently of encoder bitrate") && ok;
    return ok;
}

bool test_congestion_controller_ignores_stale_transport_pressure() {
    redclaw::net::MediaCongestionController controller;
    redclaw::net::MediaCongestionSample sample;
    sample.now_steady_ms = 1000;
    sample.encoder_target_bitrate_kbps = 8000;
    sample.transport.feedback_fresh = false;
    sample.transport.loss_per_mille = 900;
    sample.transport.queue_delay_ms = 1000;

    const auto stale = controller.update(sample);
    bool ok = expect_true(
        !stale.backoff
            && !stale.isolated_loss
            && stale.pressure == redclaw::net::MediaNetworkPressure::kStable
            && stale.pacing_bitrate_kbps == 8000,
        "stale receiver loss and queue samples must not repeatedly reduce a static stream");

    sample.now_steady_ms = 1100;
    sample.rtt_queue_delay_ms = 220;
    sample.rtt_sample_id = 1;
    sample.rtt_fresh = true;
    const auto local_pressure = controller.update(sample);
    ok = expect_true(
        local_pressure.backoff
            && local_pressure.pressure == redclaw::net::MediaNetworkPressure::kSevere,
        "fresh Host-owned RTT queue growth should still trigger backoff without receiver feedback") && ok;
    return ok;
}

bool test_pacing_budget_and_latest_only_depth_are_bounded() {
    redclaw::net::MediaPacingBudget budget;
    budget.update_rate(1000, 1000000);
    bool ok = true;
    ok = expect_true(
        budget.consume(16440, 1000000),
        "the pacer should start with at least one fragment of burst credit") && ok;
    ok = expect_true(
        !budget.consume(16440, 1000000),
        "the token bucket should not allow a second immediate fragment at 1 Mbps") && ok;
    ok = expect_true(
        budget.delay_until_available_us(16440, 1000000) > 100000,
        "fake-clock pacing should expose the delay until the next fragment") && ok;
    ok = expect_true(
        budget.consume(16440, 1200000),
        "advancing the fake clock should replenish bounded tokens") && ok;

    redclaw::net::DesktopMediaSendPacer pacer;
    std::atomic<std::uint64_t> channel_deadline_drops{0};
    ok = expect_true(
        pacer.start(
            [](std::span<const std::uint8_t>) {
                return redclaw::net::MediaPacerSendResult{.accepted = true};
            },
            []() {
                return redclaw::net::MediaPacerTransportState{
                    .open = false,
                    .buffered_amount = 0,
                };
            },
            [](const redclaw::net::SentMediaTransportPacket&) {},
            [&](const redclaw::net::MediaPacerFrameEvent& event) {
                if (event.type == redclaw::net::MediaPacerFrameEventType::kDropped
                    && event.reason == "media_channel_unavailable_deadline") {
                    channel_deadline_drops.fetch_add(1);
                }
            }),
        "the single-worker pacer should start") && ok;
    pacer.update_budget(1000, 80, 0);
    const auto make_frame = [](std::uint64_t frame_id) {
        redclaw::net::PacedEncodedVideoFrame frame;
        frame.frame_id = frame_id;
        frame.rate_revision = 1;
        frame.codec = 1;
        frame.width = 1920;
        frame.height = 1080;
        frame.timestamp_ms = frame_id;
        frame.target_fps = 30;
        frame.target_bitrate_kbps = 1000;
        frame.keyframe = true;
        frame.payload.assign(20000, 0x55);
        return frame;
    };
    auto dependent_frame = make_frame(99);
    dependent_frame.keyframe = false;
    ok = expect_true(
        !pacer.submit(std::move(dependent_frame)),
        "a dependent frame must be gated while recovery requires an IDR") && ok;
    ok = expect_true(pacer.submit(make_frame(1)), "the active frame should be accepted") && ok;
    for (int attempt = 0; attempt < 100 && pacer.telemetry().active_depth == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ok = expect_true(!pacer.submit(make_frame(2)), "an in-flight recovery IDR must not trigger repeated encoding") && ok;
    ok = expect_true(
        !pacer.submit(make_frame(3)),
        "a third queued frame must be rejected instead of growing latency") && ok;
    const auto telemetry = pacer.telemetry();
    ok = expect_true(
        telemetry.active_depth + telemetry.pending_depth <= 2
            && telemetry.pending_depth == 0,
        "recovery holds a single IDR while the capture source stays latest-only") && ok;
    for (int attempt = 0; attempt < 800 && channel_deadline_drops.load() == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ok = expect_true(
        channel_deadline_drops.load() == 1
            && pacer.telemetry().channel_deadline_drops == 1
            && pacer.telemetry().keyframe_required,
        "an unsendable frame should expire at its deadline and require a new IDR") && ok;
    pacer.reset();
    const std::uint64_t drops_after_reset = channel_deadline_drops.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    ok = expect_true(
        channel_deadline_drops.load() == drops_after_reset,
        "epoch reset should quiesce the active worker before returning") && ok;
    pacer.stop();
    return ok;
}

bool test_pacer_deadline_accounts_for_whole_frame_pacing() {
    const auto live_budget = redclaw::net::resolve_media_pacer_frame_budget(
        221149, 1769, 13);
    const auto low_rate_budget = redclaw::net::resolve_media_pacer_frame_budget(
        40917, 400, 8);
    const auto infeasible_budget = redclaw::net::resolve_media_pacer_frame_budget(
        400000, 400, 8);
    bool ok = true;
    ok = expect_true(
        live_budget.feasible
            && live_budget.pacing_duration_ms == 1001
            && live_budget.deadline_ms == 1251,
        "the live 221 KB IDR must receive its whole-frame pacing time") && ok;
    ok = expect_true(
        low_rate_budget.feasible
            && low_rate_budget.pacing_duration_ms == 819
            && low_rate_budget.deadline_ms == 1069,
        "a 40 KB recovery IDR at 400 Kbps must remain feasible") && ok;
    ok = expect_true(
        !infeasible_budget.feasible
            && infeasible_budget.deadline_ms
                == redclaw::net::kMediaPacerMaximumFrameDeadlineMs,
        "a frame that cannot finish inside the bounded window must fail preflight") && ok;
    ok = expect_true(
        redclaw::net::kEncodedVideoReassemblyTimeoutMs
            > redclaw::net::kMediaPacerMaximumFrameDeadlineMs,
        "receiver reassembly must outlive the maximum admitted sender deadline") && ok;

    const auto run_send_case = [&](std::size_t payload_bytes,
                                   std::uint32_t pacing_kbps,
                                   std::uint32_t rtt_ms,
                                   std::uint16_t expected_fragments) {
        redclaw::net::DesktopMediaSendPacer pacer;
        std::mutex event_mutex;
        std::condition_variable event_ready;
        std::optional<redclaw::net::MediaPacerFrameEvent> terminal_event;
        std::atomic<std::uint64_t> accepted_packets{0};
        bool case_ok = pacer.start(
            [](std::span<const std::uint8_t>) {
                return redclaw::net::MediaPacerSendResult{.accepted = true};
            },
            []() {
                return redclaw::net::MediaPacerTransportState{
                    .open = true,
                    .buffered_amount = 0,
                };
            },
            [&](const redclaw::net::SentMediaTransportPacket&) {
                ++accepted_packets;
                pacer.update_budget(pacing_kbps, rtt_ms, 0);
            },
            [&](const redclaw::net::MediaPacerFrameEvent& event) {
                if (event.type == redclaw::net::MediaPacerFrameEventType::kKeyframeRequired) {
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(event_mutex);
                    terminal_event = event;
                }
                event_ready.notify_one();
            });
        if (!case_ok) {
            return false;
        }
        pacer.update_budget(pacing_kbps, rtt_ms, 0);
        redclaw::net::PacedEncodedVideoFrame frame;
        frame.frame_id = 1;
        frame.rate_revision = 1;
        frame.codec = 1;
        frame.width = 1448;
        frame.height = 814;
        frame.timestamp_ms = 1;
        frame.target_fps = 1;
        frame.target_bitrate_kbps = pacing_kbps;
        frame.keyframe = true;
        frame.payload.assign(payload_bytes, 0x55);
        case_ok = pacer.submit(std::move(frame));
        std::unique_lock<std::mutex> lock(event_mutex);
        case_ok = event_ready.wait_for(
            lock,
            std::chrono::milliseconds(
                redclaw::net::kMediaPacerMaximumFrameDeadlineMs + 500),
            [&]() { return terminal_event.has_value(); }) && case_ok;
        const auto event = terminal_event;
        lock.unlock();
        const auto telemetry = pacer.telemetry();
        pacer.stop();
        return case_ok && event.has_value()
            && event->type == redclaw::net::MediaPacerFrameEventType::kSent
            && event->sent_fragments == expected_fragments
            && accepted_packets.load() == expected_fragments
            && telemetry.deadline_drops == 0
            && telemetry.frame_budget_rejections == 0;
    };

    ok = expect_true(
        run_send_case(220029, 1769, 13, 14),
        "the production pacer must complete the observed 221149-byte IDR") && ok;
    ok = expect_true(
        run_send_case(40677, 400, 8, 3),
        "the production pacer must complete a 40917-byte IDR at the pacing floor") && ok;

    redclaw::net::DesktopMediaSendPacer rejected_pacer;
    std::atomic<std::uint64_t> packets_sent{0};
    std::mutex reject_mutex;
    std::condition_variable rejected;
    std::optional<redclaw::net::MediaPacerFrameEvent> rejected_event;
    ok = expect_true(rejected_pacer.start(
        [](std::span<const std::uint8_t>) {
            return redclaw::net::MediaPacerSendResult{.accepted = true};
        },
        []() {
            return redclaw::net::MediaPacerTransportState{
                .open = true,
                .buffered_amount = 0,
            };
        },
        [&](const redclaw::net::SentMediaTransportPacket&) { ++packets_sent; },
        [&](const redclaw::net::MediaPacerFrameEvent& event) {
            if (event.type == redclaw::net::MediaPacerFrameEventType::kDropped) {
                {
                    std::lock_guard<std::mutex> lock(reject_mutex);
                    rejected_event = event;
                }
                rejected.notify_one();
            }
        }), "the infeasible-frame pacer should start") && ok;
    rejected_pacer.update_budget(400, 8, 0);
    redclaw::net::PacedEncodedVideoFrame oversized;
    oversized.frame_id = 2;
    oversized.rate_revision = 1;
    oversized.codec = 1;
    oversized.width = 1920;
    oversized.height = 1080;
    oversized.timestamp_ms = 2;
    oversized.target_fps = 1;
    oversized.target_bitrate_kbps = 400;
    oversized.keyframe = true;
    oversized.payload.assign(800000, 0x66);
    ok = expect_true(
        rejected_pacer.submit(std::move(oversized)),
        "an infeasible frame should enter bounded worker preflight") && ok;
    {
        std::unique_lock<std::mutex> lock(reject_mutex);
        ok = expect_true(
            rejected.wait_for(
                lock,
                std::chrono::milliseconds(1000),
                [&]() { return rejected_event.has_value(); }),
            "infeasible whole-frame admission should fail before fragment zero") && ok;
    }
    const auto rejected_telemetry = rejected_pacer.telemetry();
    ok = expect_true(
        rejected_event.has_value()
            && rejected_event->reason == "pacer_frame_budget_infeasible"
            && rejected_event->sent_fragments == 0
            && rejected_event->elapsed_us < 250000
            && packets_sent.load() == 0
            && rejected_telemetry.frame_budget_rejections == 1
            && rejected_telemetry.deadline_drops == 0,
        "infeasible admission must not emit a partial frame or fake a network deadline") && ok;
    rejected_pacer.stop();
    return ok;
}

bool test_estimator_expires_in_flight_without_synthesizing_loss() {
    redclaw::net::MediaTransportEstimator estimator;
    bool ok = estimator.record_sent({
        .transport_sequence = 1,
        .frame_id = 1,
        .steady_send_us = 1000000,
        .rate_revision = 1,
        .wire_bytes = 4096,
        .keyframe = true,
    });
    ok = expect_true(ok, "sent metadata should be recorded before expiration") && ok;

    const auto fresh = estimator.snapshot(1749999, 0);
    ok = expect_true(
        fresh.in_flight_bytes == 4096 && fresh.expired_in_flight_packets == 0,
        "in-flight metadata must remain authoritative before the 750 ms floor") && ok;
    const auto expired = estimator.snapshot(1750000, 0);
    ok = expect_true(
        expired.in_flight_bytes == 0
            && expired.expired_in_flight_packets == 1
            && expired.expired_in_flight_bytes == 4096
            && expired.lost_packets == 0,
        "estimator expiration must retire bytes without inventing packet loss") && ok;

    redclaw::net::MediaCongestionController controller;
    redclaw::net::MediaCongestionSample sample;
    sample.encoder_target_bitrate_kbps = 8000;
    sample.transport.feedback_fresh = true;
    sample.transport.feedback_sample_id = 9;
    sample.transport.loss_per_mille = 30;
    for (std::uint64_t now_ms : {1000ULL, 1100ULL, 1200ULL}) {
        sample.now_steady_ms = now_ms;
        const auto decision = controller.update(sample);
        ok = expect_true(!decision.backoff,
            "the same feedback sample must not accumulate repeated loss windows") && ok;
    }
    return ok;
}

bool test_reassembler_completes_frame() {
    std::vector<std::uint8_t> payload(20000);
    for (std::size_t index = 0; index < payload.size(); ++index) {
        payload[index] = static_cast<std::uint8_t>((index * 7u) & 0xffu);
    }

    redclaw::net::EncodedVideoFrameView frame;
    frame.frame_id = 71;
    frame.rate_revision = 3;
    frame.codec = 1;
    frame.width = 1280;
    frame.height = 720;
    frame.timestamp_ms = 987654;
    frame.keyframe = true;
    frame.payload = payload;

    redclaw::net::EncodedVideoFragmentPlan plan;
    std::string error;
    if (!redclaw::net::build_encoded_video_fragment_plan(frame, 4096, &plan, &error)) {
        return expect_true(false, "reassembly fragment plan should succeed: " + error);
    }

    redclaw::net::EncodedVideoFrameReassembler reassembler;
    std::vector<std::uint8_t> packet;
    redclaw::net::EncodedVideoReassemblyResult result;
    for (std::uint16_t index = 0; index < plan.fragment_count; ++index) {
        if (!redclaw::net::serialize_encoded_video_fragment(
                frame, plan, index, 200 + index, &packet, &error)) {
            return expect_true(false, "reassembly fragment serialization should succeed: " + error);
        }
        redclaw::net::EncodedVideoFragmentView fragment;
        if (!redclaw::net::parse_encoded_video_fragment(packet, &fragment, &error)) {
            return expect_true(false, "reassembly fragment parse should succeed: " + error);
        }
        result = reassembler.push(fragment, 1000 + index);
    }

    bool ok = true;
    ok = expect_true(
        result.status == redclaw::net::EncodedVideoReassemblyStatus::kComplete,
        "the final fragment should complete the frame") && ok;
    ok = expect_true(result.error.empty(), "complete reassembly should not report an error") && ok;
    ok = expect_true(result.frame.frame_id == frame.frame_id, "completed frame id should match") && ok;
    ok = expect_true(result.frame.payload == payload, "completed payload should match source bytes") && ok;
    ok = expect_true(!reassembler.has_incomplete_frame(), "completed state should be cleared") && ok;
    return ok;
}

bool test_reassembler_reports_one_error_per_dropped_frame_and_recovers() {
    std::vector<std::uint8_t> payload(20000, 0x5a);
    redclaw::net::EncodedVideoFrameView frame;
    frame.frame_id = 72;
    frame.rate_revision = 3;
    frame.codec = 1;
    frame.width = 1280;
    frame.height = 720;
    frame.timestamp_ms = 987655;
    frame.keyframe = true;
    frame.payload = payload;

    redclaw::net::EncodedVideoFragmentPlan plan;
    std::string error;
    if (!redclaw::net::build_encoded_video_fragment_plan(frame, 4096, &plan, &error)) {
        return expect_true(false, "drop fragment plan should succeed: " + error);
    }

    redclaw::net::EncodedVideoFrameReassembler reassembler;
    std::vector<std::uint8_t> packet;
    const auto push_fragment = [&](std::uint16_t index, std::uint64_t now_ms) {
        redclaw::net::EncodedVideoReassemblyResult result;
        if (!redclaw::net::serialize_encoded_video_fragment(
                frame, plan, index, 300 + index, &packet, &error)) {
            result.error = "serialize failed: " + error;
            return result;
        }
        redclaw::net::EncodedVideoFragmentView fragment;
        if (!redclaw::net::parse_encoded_video_fragment(packet, &fragment, &error)) {
            result.error = "parse failed: " + error;
            return result;
        }
        return reassembler.push(fragment, now_ms);
    };

    bool ok = true;
    const auto first = push_fragment(0, 2000);
    ok = expect_true(first.error.empty(), "first fragment should start reassembly") && ok;

    const auto after_gap = push_fragment(2, 2002);
    ok = expect_true(
        after_gap.status == redclaw::net::EncodedVideoReassemblyStatus::kDiscarded,
        "a fragment gap should discard the current frame") && ok;
    ok = expect_true(
        after_gap.error == "encoded video fragment sequence mismatch",
        "the first fragment after a gap should report one sequence error") && ok;
    ok = expect_true(
        after_gap.dropped_frame_id == 72,
        "a fragment gap should identify the damaged frame") && ok;

    const auto same_dropped_frame = push_fragment(3, 2003);
    ok = expect_true(
        same_dropped_frame.status == redclaw::net::EncodedVideoReassemblyStatus::kDiscarded,
        "later fragments from the discarded frame should stay discarded") && ok;
    ok = expect_true(
        same_dropped_frame.error.empty(),
        "later fragments from the discarded frame should not amplify the same error") && ok;

    frame.frame_id = 73;
    frame.timestamp_ms = 987656;
    frame.keyframe = true;
    redclaw::net::EncodedVideoReassemblyResult recovered;
    for (std::uint16_t index = 0; index < plan.fragment_count; ++index) {
        recovered = push_fragment(index, 3000 + index);
    }
    ok = expect_true(
        recovered.status == redclaw::net::EncodedVideoReassemblyStatus::kComplete,
        "the next complete frame should recover without waiting for a timeout") && ok;
    ok = expect_true(recovered.error.empty(), "recovered frame should not report an error") && ok;
    ok = expect_true(recovered.frame.frame_id == frame.frame_id, "recovered frame id should match") && ok;
    ok = expect_true(recovered.frame.payload == payload, "recovered frame payload should match") && ok;
    return ok;
}

bool test_reassembler_drops_dependent_frames_until_latest_keyframe_completes() {
    std::vector<std::uint8_t> payload(12000, 0x31);
    redclaw::net::EncodedVideoFrameView frame;
    frame.frame_id = 80;
    frame.rate_revision = 4;
    frame.codec = 1;
    frame.width = 1280;
    frame.height = 720;
    frame.timestamp_ms = 10000;
    frame.keyframe = true;
    frame.payload = payload;

    redclaw::net::EncodedVideoFragmentPlan plan;
    std::string error;
    if (!redclaw::net::build_encoded_video_fragment_plan(frame, 4096, &plan, &error)) {
        return expect_true(false, "dependency-drop fragment plan should succeed: " + error);
    }

    redclaw::net::EncodedVideoFrameReassembler reassembler;
    std::vector<std::uint8_t> packet;
    const auto push_fragment = [&](std::uint16_t index, std::uint64_t now_ms) {
        redclaw::net::EncodedVideoReassemblyResult result;
        if (!redclaw::net::serialize_encoded_video_fragment(
                frame, plan, index, 400 + index, &packet, &error)) {
            result.error = "serialize failed: " + error;
            return result;
        }
        redclaw::net::EncodedVideoFragmentView fragment;
        if (!redclaw::net::parse_encoded_video_fragment(packet, &fragment, &error)) {
            result.error = "parse failed: " + error;
            return result;
        }
        return reassembler.push(fragment, now_ms);
    };

    bool ok = true;
    redclaw::net::EncodedVideoReassemblyResult initial_keyframe;
    for (std::uint16_t index = 0; index < plan.fragment_count; ++index) {
        initial_keyframe = push_fragment(index, 3900 + index);
    }
    ok = expect_true(
        initial_keyframe.status == redclaw::net::EncodedVideoReassemblyStatus::kComplete
            && !reassembler.requires_keyframe(),
        "a complete initial keyframe should establish decoder dependencies") && ok;

    frame.frame_id = 81;
    frame.timestamp_ms = 10001;
    frame.keyframe = false;
    const auto first_delta = push_fragment(0, 4000);
    ok = expect_true(first_delta.started_frame, "the first delta frame should start") && ok;

    frame.frame_id = 82;
    frame.timestamp_ms = 10002;
    const auto newer_delta = push_fragment(0, 4001);
    ok = expect_true(
        newer_delta.status == redclaw::net::EncodedVideoReassemblyStatus::kDiscarded,
        "a newer dependent frame should be discarded after abandoning an incomplete delta") && ok;
    ok = expect_true(
        newer_delta.dropped_incomplete_frames == 1
            && newer_delta.dropped_dependency_frames == 1
            && newer_delta.dropped_frame_id == 81,
        "both the incomplete frame and its dependent replacement should be counted") && ok;
    ok = expect_true(
        newer_delta.keyframe_required && reassembler.requires_keyframe(),
        "the receiver should wait for a keyframe after any incomplete delta") && ok;

    const auto ignored_delta_tail = push_fragment(1, 4002);
    ok = expect_true(
        ignored_delta_tail.status == redclaw::net::EncodedVideoReassemblyStatus::kDiscarded
            && ignored_delta_tail.error.empty(),
        "remaining fragments from a rejected dependent frame should be ignored once") && ok;

    frame.frame_id = 83;
    frame.timestamp_ms = 10003;
    frame.keyframe = true;
    redclaw::net::EncodedVideoReassemblyResult recovered;
    for (std::uint16_t index = 0; index < plan.fragment_count; ++index) {
        recovered = push_fragment(index, 5000 + index);
    }
    ok = expect_true(
        recovered.status == redclaw::net::EncodedVideoReassemblyStatus::kComplete,
        "the latest complete keyframe should restore the decode dependency chain") && ok;
    ok = expect_true(recovered.frame.keyframe, "the recovery frame should be a keyframe") && ok;
    ok = expect_true(!reassembler.requires_keyframe(), "a complete keyframe should clear resync") && ok;
    return ok;
}

bool test_receiver_assembly_quality_classifies_loss_pressure() {
    const auto stable = redclaw::net::evaluate_receiver_assembly_quality({
        .completed_frames = 60,
    });
    const auto mild = redclaw::net::evaluate_receiver_assembly_quality({
        .completed_frames = 99,
        .dropped_frames = 1,
    });
    const auto degraded = redclaw::net::evaluate_receiver_assembly_quality({
        .completed_frames = 8,
        .dropped_frames = 2,
    });
    const auto severe_keyframe = redclaw::net::evaluate_receiver_assembly_quality({
        .completed_frames = 59,
        .dropped_frames = 1,
        .dropped_keyframes = 1,
    });

    bool ok = true;
    ok = expect_true(
        stable.pressure == redclaw::net::ReceiverAssemblyPressure::kStable
            && stable.loss_per_mille == 0,
        "complete assembly should be stable") && ok;
    ok = expect_true(
        mild.pressure == redclaw::net::ReceiverAssemblyPressure::kMild
            && mild.loss_per_mille == 10
            && mild.fps_scale_percent == 80,
        "isolated incomplete frames should create mild pressure") && ok;
    ok = expect_true(
        degraded.pressure == redclaw::net::ReceiverAssemblyPressure::kDegraded
            && degraded.loss_per_mille == 200
            && degraded.fps_scale_percent == 50,
        "sustained assembly loss should create degraded pressure") && ok;
    ok = expect_true(
        severe_keyframe.pressure == redclaw::net::ReceiverAssemblyPressure::kSevere
            && severe_keyframe.fps_scale_percent == 25,
        "any dropped keyframe should create immediate severe pressure") && ok;
    return ok;
}

bool test_receiver_decode_capacity_reduces_fps_and_recovers_slowly() {
    redclaw::net::ReceiverDecodeCapacityController controller;
    const redclaw::net::ReceiverDecodeCapacitySample pressure_sample {
        .reassembled_frames = 24,
        .decoded_frames = 12,
        .window_ms = 1000,
        .current_target_fps = 24,
        .maximum_target_fps = 30,
        .revision_consistent = true,
        .source_active = true,
        .network_stable = true,
    };

    auto decision = controller.update(pressure_sample);
    bool ok = expect_true(
        !decision.valid && !decision.target_changed,
        "an immature receiver window must not retarget");
    for (int window = 1; window < 10; ++window) {
        decision = controller.update(pressure_sample);
    }
    ok = expect_true(
        decision.target_changed && decision.target_fps == 12
            && decision.observed_decode_fps == 12
            && decision.decrease_total == 1,
        "two mature pressure windows must reduce FPS to measured decode capacity") && ok;

    redclaw::net::ReceiverDecodeCapacitySample stable_sample {
        .reassembled_frames = 12,
        .decoded_frames = 12,
        .window_ms = 1000,
        .current_target_fps = 12,
        .maximum_target_fps = 30,
        .revision_consistent = true,
        .source_active = true,
        .network_stable = true,
    };
    for (int window = 0; window < 4; ++window) {
        decision = controller.update(stable_sample);
        ok = expect_true(!decision.target_changed,
            "receiver decode recovery must wait five stable windows") && ok;
    }
    decision = controller.update(stable_sample);
    ok = expect_true(
        decision.target_changed && decision.target_fps == 13
            && decision.increase_total == 1,
        "the fifth stable network window may restore exactly one FPS") && ok;

    stable_sample.revision_consistent = false;
    decision = controller.update(stable_sample);
    ok = expect_true(
        !decision.valid && !decision.target_changed && decision.target_fps == 12,
        "old epoch or rate revision samples must not change FPS") && ok;

    stable_sample.revision_consistent = true;
    stable_sample.reassembled_frames = std::numeric_limits<std::uint64_t>::max();
    stable_sample.decoded_frames = std::numeric_limits<std::uint64_t>::max();
    decision = controller.update(stable_sample);
    ok = expect_true(
        !decision.valid && !decision.pressure,
        "unbounded counter deltas must be rejected without overflow") && ok;
    return ok;
}

bool test_stream_target_reduction_saturates_without_unsigned_wrap() {
    return expect_true(
               redclaw::net::saturating_reduce_stream_target_fps(30, 3) == 27,
               "normal target reduction should subtract the requested step")
        && expect_true(
            redclaw::net::saturating_reduce_stream_target_fps(3, 3) == 1,
            "a reduction equal to the current target should stop at one fps")
        && expect_true(
            redclaw::net::saturating_reduce_stream_target_fps(2, 3) == 1,
            "a larger reduction must not wrap an unsigned two-fps target")
        && expect_true(
            redclaw::net::saturating_reduce_stream_target_fps(1, 3) == 1,
            "one fps must remain the target floor")
        && expect_true(
            redclaw::net::saturating_reduce_stream_target_fps(0, 3) == 1,
            "an invalid zero target should fail closed to one fps");
}

bool test_rtt_pressure_uses_queue_delay_above_session_baseline() {
    const auto stable_high_latency = redclaw::net::evaluate_stream_rtt_signal(
        181, 178, 40, 120, 200);
    const auto severe_queue = redclaw::net::evaluate_stream_rtt_signal(
        300, 100, 40, 120, 200);
    const auto high_queue = redclaw::net::evaluate_stream_rtt_signal(
        220, 100, 40, 120, 200);
    const auto relief = redclaw::net::evaluate_stream_rtt_signal(
        139, 100, 40, 120, 200);
    const auto neutral = redclaw::net::evaluate_stream_rtt_signal(
        150, 100, 40, 120, 200);
    const auto defensive_sample = redclaw::net::evaluate_stream_rtt_signal(
        99, 100, 40, 120, 200);
    const auto missing_sample = redclaw::net::evaluate_stream_rtt_signal(
        0, 0, 40, 120, 200);
    const auto invalid_thresholds = redclaw::net::evaluate_stream_rtt_signal(
        200, 100, 121, 120, 200);

    return expect_true(
               stable_high_latency.valid
                   && stable_high_latency.baseline_rtt_ms == 178
                   && stable_high_latency.queue_delay_ms == 3
                   && stable_high_latency.relief
                   && !stable_high_latency.high_pressure
                   && !stable_high_latency.severe_pressure,
               "a high path baseline without queue growth must not look congested")
        && expect_true(
            severe_queue.valid
                && severe_queue.queue_delay_ms == 200
                && severe_queue.high_pressure
                && severe_queue.severe_pressure,
            "two hundred milliseconds above baseline should be severe pressure")
        && expect_true(
            high_queue.valid
                && high_queue.queue_delay_ms == 120
                && high_queue.high_pressure
                && !high_queue.severe_pressure,
            "the high queue-delay boundary should create non-severe pressure")
        && expect_true(
            relief.valid && relief.queue_delay_ms == 39 && relief.relief,
            "queue delay below the relief threshold should permit recovery")
        && expect_true(
            neutral.valid
                && neutral.queue_delay_ms == 50
                && !neutral.relief
                && !neutral.high_pressure,
            "the hysteresis band should be neither pressure nor relief")
        && expect_true(
            defensive_sample.valid
                && defensive_sample.queue_delay_ms == 0
                && defensive_sample.relief,
            "a sample below the remembered minimum must saturate at zero queue delay")
        && expect_true(
            !missing_sample.valid && !invalid_thresholds.valid,
            "missing RTT samples and inconsistent thresholds must fail closed");
}

bool test_encode_budget_gap_only_counts_available_source_frames() {
    using redclaw::net::AvailableEncodeBudgetPressure;

    return expect_true(
               redclaw::net::resolve_available_encode_budget_gap(0, 0, 30) == 0,
               "an idle event-driven desktop source must not look encoder-starved")
        && expect_true(
            redclaw::net::resolve_available_encode_budget_gap(10, 10, 30) == 0,
            "all available source frames encoded should have no budget gap")
        && expect_true(
            redclaw::net::resolve_available_encode_budget_gap(30, 10, 30) == 20,
            "available captured frames not encoded should create pressure")
        && expect_true(
            redclaw::net::resolve_available_encode_budget_gap(60, 20, 30) == 10,
            "the gap should remain bounded by the target pacing budget")
        && expect_true(
            redclaw::net::evaluate_available_encode_budget_pressure(234, 228, 300)
                == AvailableEncodeBudgetPressure::kStable,
            "a small high-rate latest-only merge must not look like encoder pressure")
        && expect_true(
            redclaw::net::evaluate_available_encode_budget_pressure(30, 27, 30)
                == AvailableEncodeBudgetPressure::kPressure,
            "a ten-percent available-budget gap should create pressure")
        && expect_true(
            redclaw::net::evaluate_available_encode_budget_pressure(30, 23, 30)
                == AvailableEncodeBudgetPressure::kSevere,
            "a sustained twenty-percent-plus available-budget gap should be severe");
}

bool test_sent_metadata_ring_and_feedback_freshness() {
    redclaw::net::SentVideoFrameMetadataRing ring(2);
    bool ok = true;
    ok = expect_true(
        ring.record({
            .frame_id = 10,
            .steady_send_time_ms = 1000,
            .rate_revision = 4,
            .fps = 30,
            .bitrate_kbps = 8000,
            .keyframe = true,
        }),
        "valid sent-frame metadata should be recorded") && ok;
    ok = expect_true(
        ring.record({
            .frame_id = 11,
            .steady_send_time_ms = 1100,
            .rate_revision = 4,
            .fps = 30,
            .bitrate_kbps = 8000,
            .keyframe = false,
        }),
        "a newer metadata record should be accepted") && ok;

    const auto fresh = redclaw::net::assess_receiver_feedback(
        ring.find(11), 1320, 80, 4, 4);
    ok = expect_true(
        fresh.freshness == redclaw::net::ReceiverFeedbackFreshness::kFresh
            && fresh.feedback_age_ms == 220
            && fresh.fresh_limit_ms == 250,
        "feedback inside max(2*SRTT, 250ms) should be fresh") && ok;

    const auto delayed = redclaw::net::assess_receiver_feedback(
        ring.find(11), 1700, 80, 4, 4);
    ok = expect_true(
        delayed.freshness == redclaw::net::ReceiverFeedbackFreshness::kDelayed,
        "feedback between the fresh and expiry limits should be delayed") && ok;

    const auto expired = redclaw::net::assess_receiver_feedback(
        ring.find(11), 2200, 80, 4, 4);
    ok = expect_true(
        expired.freshness == redclaw::net::ReceiverFeedbackFreshness::kExpired,
        "old feedback should expire without another rate reduction") && ok;

    const auto stale_revision = redclaw::net::assess_receiver_feedback(
        ring.find(11), 1200, 80, 4, 5);
    ok = expect_true(
        stale_revision.freshness == redclaw::net::ReceiverFeedbackFreshness::kStaleRevision,
        "feedback for an older target revision should be ignored") && ok;

    ok = expect_true(
        ring.record({
            .frame_id = 12,
            .steady_send_time_ms = 1200,
            .rate_revision = 5,
            .fps = 15,
            .bitrate_kbps = 5000,
            .keyframe = true,
        }) && !ring.find(10).has_value() && ring.size() == 2,
        "the ring should evict only metadata and stay bounded") && ok;
    return ok;
}

bool test_stream_health_uses_role_owned_evidence() {
    using redclaw::net::DesktopStreamEndpointRole;
    using redclaw::net::DesktopStreamHealth;
    using redclaw::net::DesktopStreamHealthSample;

    bool ok = true;
    ok = expect_true(
        redclaw::net::classify_desktop_stream_health({
            .role = DesktopStreamEndpointRole::kHost,
            .direct_nat_path = true,
            .transmitted_frames = 30,
            .received_frames = 0,
        }) == DesktopStreamHealth::kDirectNatPath,
        "a Host must not infer network loss from its intentionally empty receive counter") && ok;
    ok = expect_true(
        redclaw::net::classify_desktop_stream_health({
            .role = DesktopStreamEndpointRole::kHost,
            .direct_nat_path = true,
            .receiver_loss_per_mille = 25,
            .transmitted_frames = 30,
        }) == DesktopStreamHealth::kNetworkLoss,
        "a Host should classify revision-matched receiver loss as network evidence") && ok;
    ok = expect_true(
        redclaw::net::classify_desktop_stream_health({
            .role = DesktopStreamEndpointRole::kController,
            .direct_nat_path = true,
            .received_frames = 30,
            .rendered_frames = 0,
            .direct_pipe_connected = true,
            .direct_pipe_writer_frames = 30,
            .direct_pipe_reader_frames = 29,
            .direct_pipe_backlog = 1,
        }) == DesktopStreamHealth::kDirectNatPath,
        "one asynchronous latest frame is not playback backlog and runtime render stays irrelevant") && ok;
    ok = expect_true(
        redclaw::net::classify_desktop_stream_health({
            .role = DesktopStreamEndpointRole::kController,
            .received_frames = 30,
            .direct_pipe_connected = true,
            .direct_pipe_writer_frames = 5,
            .direct_pipe_reader_frames = 2,
            .direct_pipe_backlog = 2,
        }) == DesktopStreamHealth::kPlaybackBacklog,
        "direct-pipe writer progress without matching reader progress is playback backlog") && ok;
    ok = expect_true(
        redclaw::net::classify_desktop_stream_health({
            .role = DesktopStreamEndpointRole::kController,
            .received_frames = 1,
            .direct_pipe_connected = true,
            .direct_pipe_write_failures = 1,
        }) == DesktopStreamHealth::kDirectPipeWriteFailures,
        "fatal direct-pipe writes must remain visible") && ok;
    return ok;
}

bool test_stream_health_rejects_transport_only_startup() {
    using redclaw::net::DesktopStreamEndpointRole;
    using redclaw::net::DesktopStreamHealth;

    bool ok = true;
    const auto classify_host = [](std::uint64_t elapsed_ms,
                                  std::uint32_t captured,
                                  std::uint32_t submitted,
                                  std::uint32_t encoded,
                                  std::uint32_t transmitted) {
        return redclaw::net::classify_desktop_stream_health({
            .role = DesktopStreamEndpointRole::kHost,
            .startup_tracking = true,
            .startup_elapsed_ms = elapsed_ms,
            .startup_timeout_ms = 10000,
            .captured_frames_total = captured,
            .encode_submit_attempts_total = submitted,
            .encoded_frames_total = encoded,
            .transmitted_frames_total = transmitted,
            .direct_nat_path = true,
            .transmitted_frames = transmitted,
        });
    };

    ok = expect_true(
        classify_host(9999, 0, 0, 0, 0) == DesktopStreamHealth::kIdle,
        "the first-media deadline must not fail a healthy startup early") && ok;
    ok = expect_true(
        classify_host(10000, 0, 0, 0, 0) == DesktopStreamHealth::kCaptureStartupStalled,
        "open channels without a captured frame must be a bounded startup failure") && ok;
    ok = expect_true(
        classify_host(10000, 15, 1, 0, 0) == DesktopStreamHealth::kEncoderStartupStalled,
        "a submitted first frame with no encoded output must not be reported healthy") && ok;
    ok = expect_true(
        classify_host(10000, 15, 1, 1, 0) == DesktopStreamHealth::kTransmitStartupStalled,
        "an encoded first keyframe that never transmits must be classified separately") && ok;
    ok = expect_true(
        classify_host(10000, 15, 1, 1, 1) == DesktopStreamHealth::kDirectNatPath,
        "actual transmitted media should clear the Host startup gate") && ok;

    ok = expect_true(
        redclaw::net::classify_desktop_stream_health({
            .role = DesktopStreamEndpointRole::kController,
            .startup_tracking = true,
            .startup_elapsed_ms = 10000,
            .startup_timeout_ms = 10000,
            .direct_nat_path = true,
        }) == DesktopStreamHealth::kReceiveStartupStalled,
        "a connected Controller with zero media must not be reported healthy") && ok;
    ok = expect_true(
        redclaw::net::classify_desktop_stream_health({
            .role = DesktopStreamEndpointRole::kController,
            .startup_tracking = true,
            .startup_elapsed_ms = 10000,
            .startup_timeout_ms = 10000,
            .received_frames_total = 4,
            .reassembled_frames_total = 1,
            .direct_nat_path = true,
        }) == DesktopStreamHealth::kPlaybackStartupStalled,
        "received fragments without a delivered frame must fail the playback startup gate") && ok;
    ok = expect_true(
        redclaw::net::classify_desktop_stream_health({
            .role = DesktopStreamEndpointRole::kController,
            .startup_tracking = true,
            .startup_elapsed_ms = 10000,
            .startup_timeout_ms = 10000,
            .received_frames_total = 4,
            .reassembled_frames_total = 1,
            .delivered_frames_total = 1,
            .direct_nat_path = true,
            .received_frames = 1,
            .direct_pipe_connected = true,
            .direct_pipe_writer_frames = 1,
            .direct_pipe_reader_frames = 1,
        }) == DesktopStreamHealth::kDirectNatPath,
        "a delivered Controller frame should clear the playback startup gate") && ok;
    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_fragment_round_trip_reuses_packet_capacity() && ok;
    ok = test_rejects_invalid_inputs() && ok;
    ok = test_navigation_thumbnail_is_bounded_and_latest_ready() && ok;
    ok = test_transport_feedback_recorder_batches_and_resets() && ok;
    ok = test_transport_estimator_tracks_rate_gap_queue_and_replay() && ok;
    ok = test_stale_revision_feedback_retires_only_old_in_flight_packets() && ok;
    ok = test_congestion_controller_avoids_double_loss_backoff_and_probes() && ok;
    ok = test_congestion_controller_ignores_stale_transport_pressure() && ok;
    ok = test_pacing_budget_and_latest_only_depth_are_bounded() && ok;
    ok = test_pacer_deadline_accounts_for_whole_frame_pacing() && ok;
    ok = test_estimator_expires_in_flight_without_synthesizing_loss() && ok;
    ok = test_reassembler_completes_frame() && ok;
    ok = test_reassembler_reports_one_error_per_dropped_frame_and_recovers() && ok;
    ok = test_reassembler_drops_dependent_frames_until_latest_keyframe_completes() && ok;
    ok = test_receiver_assembly_quality_classifies_loss_pressure() && ok;
    ok = test_receiver_decode_capacity_reduces_fps_and_recovers_slowly() && ok;
    ok = test_stream_target_reduction_saturates_without_unsigned_wrap() && ok;
    ok = test_rtt_pressure_uses_queue_delay_above_session_baseline() && ok;
    ok = test_encode_budget_gap_only_counts_available_source_frames() && ok;
    ok = test_sent_metadata_ring_and_feedback_freshness() && ok;
    ok = test_stream_health_uses_role_owned_evidence() && ok;
    ok = test_stream_health_rejects_transport_only_startup() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "[PASS] redclaw_net_video_frame_transport_tests\n";
    return 0;
}
