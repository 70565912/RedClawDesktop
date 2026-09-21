#include "redclaw/net/video_frame_transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include "media_transport_limits.h"

namespace redclaw::net {
namespace {
using transport_detail::kFragmentHeaderBytes;
using transport_detail::resolve_pacer_window_bytes;
using transport_detail::resolve_transport_in_flight_expiry_us;
std::uint64_t steady_now_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
struct TraceCompletion final {
    MediaFrameTraceRecorder* recorder;
    MediaFrameTrace& trace;
    ~TraceCompletion() {
        if (recorder && trace.enabled) {
            trace.finish_us = steady_now_us();
            recorder->record(trace);
        }
    }
};
}  // namespace

struct DesktopMediaSendPacer::Impl {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool running = false;
    std::uint64_t generation = 1;
    std::uint64_t writable_revision = 0;
    std::uint64_t next_transport_sequence = 0;
    std::deque<PacedEncodedVideoFrame> pending;
    std::vector<std::vector<std::uint8_t>> free_payloads;
    std::size_t active_capacity = 0;
    bool encode_reserved = false;
    std::uint64_t reservation_generation = 0;
    std::uint64_t reservation_recovery_generation = 0;
    std::uint64_t next_frame_us = 0;
    std::uint64_t frame_cadence_remainder = 0;
    std::uint32_t frame_cadence_fps = 0;
    bool application_limited = true;
    MediaSampleWindow normal_service;
    MediaSampleWindow key_service;
    MediaCongestionDecision policy;
    MediaPacerSendCallback send_callback;
    MediaPacerTransportStateCallback transport_state_callback;
    MediaPacerPacketSentCallback packet_sent_callback;
    MediaPacerFrameEventCallback frame_event_callback;
    MediaPacerCapacityCallback capacity_callback;
    bool recovery_notified = false;
    bool active_recovery_frame = false;
    std::uint64_t acknowledged_packets = 0;
    MediaRecoveryProbe recovery_probe;
    MediaPacingBudget budget;
    MediaFrameTraceRecorder* trace_recorder = nullptr; // Outlives the stopped pacer.
    MediaPacerTelemetry telemetry;
    std::size_t retained_bytes() const {
        std::size_t bytes = active_capacity;
        for (const auto& frame : pending) bytes += frame.payload.capacity();
        for (const auto& buffer : free_payloads) bytes += buffer.capacity();
        return bytes;
    }
    std::size_t reservation_growth() const {
        return encode_reserved ? 0 : transport_detail::kMaximumEncodedFrameBytes
            - (free_payloads.empty() ? 0 : free_payloads.back().capacity());
    }
    void trim_free() {
        while (!free_payloads.empty() && (free_payloads.size() > telemetry.queue_target_frames
            || retained_bytes() + telemetry.reserved_bytes + reservation_growth()
                > transport_detail::kPacerPayloadMemoryLimitBytes))
            free_payloads.pop_back();
    }
    void recycle(std::vector<std::uint8_t> buffer) {
        buffer.clear();
        if (buffer.capacity() != 0 && buffer.capacity() <= transport_detail::kMaximumEncodedFrameBytes)
            free_payloads.push_back(std::move(buffer));
        trim_free();
    }
    void refresh_queue() {
        telemetry.pending_depth = pending.size();
        telemetry.pending_bytes = 0;
        for (const auto& frame : pending) telemetry.pending_bytes += frame.payload.size();
        telemetry.retained_bytes = retained_bytes();
        const auto period = telemetry.target_fps == 0 ? 0ULL : 1000000ULL / telemetry.target_fps;
        // Keyframes are rare dependency anchors. Learning their complete send
        // duration as the normal FIFO depth turns an occasional keyframe wait
        // into persistent display latency for every following P-frame.
        const auto service = static_cast<std::uint64_t>(normal_service.quantile(0.95));
        const auto jitter = budget.lateness_us();
        telemetry.queue_target_us = std::max(period, service + jitter);
        if (telemetry.congested) {
            telemetry.queue_target_frames = 1;
            telemetry.queue_target_us = period;
        } else {
            telemetry.queue_target_frames = period == 0 ? 1 : std::clamp<std::size_t>(
                static_cast<std::size_t>((telemetry.queue_target_us + period - 1) / period),
                1, transport_detail::kPacerMaximumQueueFrames);
        }
        telemetry.queue_target_bytes = std::clamp<std::size_t>(static_cast<std::size_t>(
            static_cast<double>(telemetry.pacing_bitrate_kbps) * telemetry.queue_target_us / 8000.0),
            transport_detail::kMinimumPacerWindowBytes, transport_detail::kMaximumEncodedFrameBytes);
        trim_free();
        telemetry.retained_bytes = retained_bytes();
        telemetry.resource_limited = telemetry.retained_bytes + telemetry.reserved_bytes
            + reservation_growth()
                > transport_detail::kPacerPayloadMemoryLimitBytes;
        telemetry.buffer_target_reason = telemetry.resource_limited ? MediaBufferTargetReason::kResourceLimit
            : telemetry.congested ? MediaBufferTargetReason::kCongestion
            : service != 0 ? MediaBufferTargetReason::kMeasuredService : MediaBufferTargetReason::kBootstrap;
    }
    void clear_pending() {
        for (auto& frame : pending) recycle(std::move(frame.payload));
        pending.clear();
        refresh_queue();
    }
    MediaAdmissionResult admission_locked(std::uint64_t now_ms) const {
        MediaAdmissionResult result;
        result.budget_revision = telemetry.budget_revision;
        result.next_check_ms = telemetry.next_admission_ms;
        if (!running) result.state = MediaAdmissionState::kStopped;
        else if (encode_reserved || telemetry.resource_limited
            || pending.size() >= telemetry.queue_target_frames
            || telemetry.pending_bytes >= telemetry.queue_target_bytes
            || (!pending.empty() && now_ms * 1000 >= pending.front().enqueued_us + telemetry.queue_target_us))
            result.state = MediaAdmissionState::kWaitCapacity;
        else if (active_recovery_frame) result.state = MediaAdmissionState::kWaitRecovery;
        else if (now_ms < telemetry.next_admission_ms) result.state = MediaAdmissionState::kBudgetInfeasible;
        else result.state = MediaAdmissionState::kReady;
        return result;
    }

    void defer_budget_retry(std::size_t wire_bytes, std::uint64_t now_ms) {
        telemetry.rejected_wire_bytes = wire_bytes;
        const auto shift = std::min<std::uint64_t>(telemetry.budget_retry_attempt++, 5);
        telemetry.next_admission_ms = now_ms + std::min<std::uint64_t>(30000, 1000ULL << shift);
    }
    void release_in_flight(std::size_t wire_bytes) {
        telemetry.in_flight_bytes = telemetry.in_flight_bytes >= wire_bytes
            ? telemetry.in_flight_bytes - wire_bytes
            : 0;
    }

    void publish_event(MediaPacerFrameEvent event) {
        MediaPacerFrameEventCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback = frame_event_callback;
        }
        if (callback) {
            callback(event);
        }
    }

    void notify_capacity() {
        if (capacity_callback) {
            capacity_callback();
        }
    }

    void require_keyframe(const PacedEncodedVideoFrame& frame, std::string reason,
                          std::uint64_t frame_generation, bool notify_encoder = true) {
        bool notify_recovery = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!running || generation != frame_generation) {
                return;
            }
            if (!telemetry.keyframe_required) {
                ++telemetry.recovery_generation;
            }
            telemetry.keyframe_required = true;
            // Preserve only a queued independent suffix. Dropping a reference
            // invalidates every following predictive frame until the next IDR.
            while (!pending.empty() && !pending.front().keyframe) {
                recycle(std::move(pending.front().payload));
                pending.pop_front();
                ++telemetry.dependency_pending_drops;
            }
            for (auto& queued : pending) queued.recovery_generation = telemetry.recovery_generation;
            refresh_queue();
            notify_recovery = !recovery_notified && notify_encoder;
            recovery_notified |= notify_recovery;
            if (!notify_recovery) {
                ++telemetry.suppressed_keyframe_requests;
            }
        }
        notify_capacity();
        if (!notify_recovery) {
            return;
        }
        publish_event({
            .type = MediaPacerFrameEventType::kKeyframeRequired,
            .frame_id = frame.frame_id,
            .rate_revision = frame.rate_revision,
            .target_fps = frame.target_fps,
            .target_bitrate_kbps = frame.target_bitrate_kbps,
            .keyframe_refresh_generation = frame.keyframe_refresh_generation,
            .keyframe = frame.keyframe,
            .reason = std::move(reason),
        });
    }

    void send_frame(PacedEncodedVideoFrame& frame, std::uint64_t frame_generation) {
        auto trace = frame.trace;
        const bool tracing = trace_recorder && trace.enabled;
        TraceCompletion trace_completion{trace_recorder, trace};
        if (tracing) {
            trace.pacer_begin_us = steady_now_us();
            trace.outcome = 3;
            trace.frame_id = frame.frame_id;
            trace.rate_revision = frame.rate_revision;
            trace.keyframe = frame.keyframe;
            trace.width = frame.width;
            trace.height = frame.height;
            trace.target_fps = frame.target_fps;
        }
        EncodedVideoFrameView view;
        view.frame_id = frame.frame_id;
        view.rate_revision = frame.rate_revision;
        view.capture_region_revision = frame.capture_region_revision;
        view.codec = frame.codec;
        view.width = frame.width;
        view.height = frame.height;
        view.timestamp_ms = frame.timestamp_ms;
        view.keyframe = frame.keyframe;
        view.content_rect_x = frame.content_rect_x;
        view.content_rect_y = frame.content_rect_y;
        view.content_rect_width = frame.content_rect_width;
        view.content_rect_height = frame.content_rect_height;
        view.payload = frame.payload;

        EncodedVideoFragmentPlan plan;
        std::string error;
        if (!build_encoded_video_fragment_plan(view, 16 * 1024, &plan, &error)) {
            trace.outcome = 2;
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++telemetry.send_failures;
            }
            publish_event({
                .type = MediaPacerFrameEventType::kDropped,
                .frame_id = frame.frame_id,
                .rate_revision = frame.rate_revision,
                .target_fps = frame.target_fps,
                .target_bitrate_kbps = frame.target_bitrate_kbps,
                .keyframe_refresh_generation = frame.keyframe_refresh_generation,
                .keyframe = frame.keyframe,
                .reason = error,
            });
            require_keyframe(frame, "packetization_failed", frame_generation);
            return;
        }

        const std::uint64_t started_us = steady_now_us();
        std::uint32_t rtt_ms = 0;
        std::uint32_t pacing_bitrate_kbps = 0;
        std::size_t initial_in_flight_bytes = 0;
        std::uint64_t budget_revision = 0;
        std::uint64_t probe_generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            rtt_ms = telemetry.smoothed_rtt_ms;
            pacing_bitrate_kbps = telemetry.pacing_bitrate_kbps;
            initial_in_flight_bytes = telemetry.in_flight_bytes;
            budget_revision = telemetry.budget_revision;
            if (frame.recovery_frame && recovery_probe.phase == MediaRecoveryProbePhase::kProbing) {
                probe_generation = recovery_probe.generation;
            }
        }
        const auto frame_budget = resolve_media_pacer_frame_budget(
            plan.wire_bytes,
            pacing_bitrate_kbps,
            rtt_ms, frame.recovery_frame);
        if (tracing) {
            trace.wire_bytes = plan.wire_bytes;
            trace.fragment_count = plan.fragment_count;
            trace.pacing_kbps = pacing_bitrate_kbps;
            trace.rtt_ms = rtt_ms;
        }
        if (!frame_budget.feasible && probe_generation == 0) {
            trace.outcome = 2;
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++telemetry.frame_budget_rejections;
                defer_budget_retry(plan.wire_bytes, started_us / 1000);
            }
            publish_event({
                .type = MediaPacerFrameEventType::kDropped,
                .frame_id = frame.frame_id,
                .rate_revision = frame.rate_revision,
                .target_fps = frame.target_fps,
                .target_bitrate_kbps = frame.target_bitrate_kbps,
                .keyframe_refresh_generation = frame.keyframe_refresh_generation,
                .keyframe = frame.keyframe,
                .sent_fragments = 0,
                .fragment_count = plan.fragment_count,
                .wire_bytes = plan.wire_bytes,
                .pacing_bitrate_kbps = pacing_bitrate_kbps,
                .pacing_duration_ms = frame_budget.pacing_duration_ms,
                .deadline_ms = frame_budget.deadline_ms,
                .elapsed_us = steady_now_us() - started_us,
                .reason = "pacer_frame_budget_infeasible",
            });
            require_keyframe(frame, "pacer_frame_budget_infeasible", frame_generation, false);
            return;
        }
        const auto maximum_ms = frame.recovery_frame
            ? kMediaPacerRecoveryFrameDeadlineMs : kMediaPacerMaximumFrameDeadlineMs;
        const auto hard_deadline_us = started_us + maximum_ms * 1000ULL;
        std::uint64_t deadline_ms = std::min(maximum_ms, frame_budget.deadline_ms
            + (initial_in_flight_bytes > 0 ? resolve_transport_in_flight_expiry_us(rtt_ms) / 1000ULL : 0));
        {
            std::lock_guard lock(mutex);
            deadline_ms = std::min(maximum_ms, std::max(deadline_ms,
                frame_budget.pacing_duration_ms + (3 * policy.feedback_horizon_us + 999) / 1000));
        }
        std::uint64_t deadline_us = started_us + deadline_ms * 1000ULL;
        std::size_t sent_wire_bytes = 0;

        std::vector<std::uint8_t> packet;
        packet.reserve(16 * 1024);
        std::uint16_t sent_fragments = 0;
        std::string deadline_reason = "pacer_token_deadline";
        for (std::uint16_t fragment_index = 0;
             fragment_index < plan.fragment_count;
             ++fragment_index) {
            const std::size_t payload_offset = static_cast<std::size_t>(fragment_index)
                * plan.max_fragment_payload_bytes;
            const std::size_t wire_bytes = kFragmentHeaderBytes + std::min(
                frame.payload.size() - payload_offset,
                plan.max_fragment_payload_bytes);
            std::uint64_t transport_sequence = 0;
            while (true) {
                std::uint64_t now_us = steady_now_us();
                {
                    std::lock_guard lock(mutex);
                    if (!running || generation != frame_generation) return;
                    if (budget_revision != telemetry.budget_revision) {
                        budget_revision = telemetry.budget_revision;
                        pacing_bitrate_kbps = telemetry.pacing_bitrate_kbps;
                        if (tracing) trace.pacing_kbps = pacing_bitrate_kbps;
                        rtt_ms = telemetry.smoothed_rtt_ms;
                        const auto remaining = resolve_media_pacer_frame_budget(
                            plan.wire_bytes - sent_wire_bytes, pacing_bitrate_kbps, rtt_ms,
                            frame.recovery_frame);
                        const auto token_delay = budget.delay_until_available_us(plan.wire_bytes - sent_wire_bytes, now_us);
                        const auto in_flight_delay = telemetry.in_flight_bytes + wire_bytes > telemetry.in_flight_limit_bytes
                            ? resolve_transport_in_flight_expiry_us(rtt_ms) : 0ULL;
                        const auto remaining_us = token_delay == std::numeric_limits<std::uint64_t>::max()
                            ? maximum_ms * 1000ULL
                            : token_delay + remaining.guard_duration_ms * 1000ULL + in_flight_delay;
                        deadline_us = std::min(hard_deadline_us, now_us + remaining_us);
                        deadline_ms = (deadline_us - started_us) / 1000;
                    }
                    // Probe completion only changes the rate used for later
                    // packets. A failed capacity probe must not invalidate an
                    // otherwise sendable recovery frame at the confirmed rate.
                }
                if (now_us >= deadline_us) {
                    trace.outcome = 2;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        ++telemetry.deadline_drops;
                        if (deadline_reason == "pacer_token_deadline" || probe_generation != 0) {
                            defer_budget_retry(plan.wire_bytes, now_us / 1000);
                        }
                        if (deadline_reason == "pacer_token_deadline") {
                            ++telemetry.pacing_token_deadline_drops;
                        } else if (deadline_reason == "pacer_in_flight_deadline") {
                            ++telemetry.in_flight_deadline_drops;
                        } else if (deadline_reason == "data_channel_buffer_deadline") {
                            ++telemetry.buffered_deadline_drops;
                        } else if (deadline_reason == "media_channel_unavailable_deadline") {
                            ++telemetry.channel_deadline_drops;
                        }
                    }
                    publish_event({
                        .type = MediaPacerFrameEventType::kDropped,
                        .frame_id = frame.frame_id,
                        .rate_revision = frame.rate_revision,
                        .target_fps = frame.target_fps,
                        .target_bitrate_kbps = frame.target_bitrate_kbps,
                        .keyframe_refresh_generation = frame.keyframe_refresh_generation,
                        .keyframe = frame.keyframe,
                        .sent_fragments = sent_fragments,
                        .fragment_count = plan.fragment_count,
                        .wire_bytes = plan.wire_bytes,
                        .pacing_bitrate_kbps = pacing_bitrate_kbps,
                        .pacing_duration_ms = frame_budget.pacing_duration_ms,
                        .deadline_ms = deadline_ms,
                        .elapsed_us = now_us - started_us,
                        .reason = deadline_reason,
                    });
                    require_keyframe(frame, deadline_reason, frame_generation,
                        deadline_reason != "pacer_token_deadline" && probe_generation == 0);
                    return;
                }

                MediaPacerTransportState transport_state;
                const auto state_begin_us = tracing ? steady_now_us() : 0;
                if (transport_state_callback) {
                    transport_state = transport_state_callback();
                }
                if (tracing) trace.transport_state_us += steady_now_us() - state_begin_us;
                std::unique_lock<std::mutex> lock(mutex);
                if (!running || generation != frame_generation) {
                    return;
                }
                now_us = steady_now_us();
                if (now_us >= deadline_us) continue;
                const auto probe_limit = recovery_probe.wire_budget_bytes == 0
                    ? 4U * 16U * 1024U : recovery_probe.wire_budget_bytes;
                const bool probe_packet = recovery_probe.phase == MediaRecoveryProbePhase::kProbing
                    && telemetry.probe_end_sequence == 0
                    && telemetry.probe_wire_bytes + wire_bytes <= probe_limit;
                probe_generation = probe_packet ? recovery_probe.generation : 0;
                const auto effective_rate_kbps = probe_packet
                    ? telemetry.pacing_bitrate_kbps
                    : (recovery_probe.phase == MediaRecoveryProbePhase::kProbing
                            && recovery_probe.baseline_rate_kbps != 0
                        ? recovery_probe.baseline_rate_kbps : telemetry.pacing_bitrate_kbps);
                budget.update_rate(effective_rate_kbps, now_us);
                const std::uint64_t delay_us = budget.delay_until_available_us(
                    wire_bytes,
                    now_us);
                const bool in_flight_available = telemetry.in_flight_bytes + wire_bytes
                    <= telemetry.in_flight_limit_bytes;
                const bool buffered_available = transport_state.open
                    && transport_state.buffered_amount + wire_bytes
                        <= telemetry.buffered_limit_bytes;
                if (delay_us == 0 && in_flight_available && buffered_available
                    && budget.consume(wire_bytes, now_us)) {
                    transport_sequence = ++next_transport_sequence;
                    telemetry.in_flight_bytes += wire_bytes;
                    if (probe_generation != 0) {
                        telemetry.probe_wire_bytes += wire_bytes;
                        const auto next_offset = payload_offset + plan.max_fragment_payload_bytes;
                        const auto next_wire_bytes = next_offset >= frame.payload.size() ? 0
                            : kFragmentHeaderBytes + std::min(frame.payload.size() - next_offset,
                                plan.max_fragment_payload_bytes);
                        // Publish the boundary before an ACK can race with it.
                        if (next_wire_bytes == 0 || telemetry.probe_wire_bytes + next_wire_bytes > probe_limit)
                            telemetry.probe_end_sequence = transport_sequence;
                    }
                    break;
                }
                if (!transport_state.open) {
                    deadline_reason = "media_channel_unavailable_deadline";
                } else if (!in_flight_available) {
                    deadline_reason = "pacer_in_flight_deadline";
                } else if (!buffered_available) {
                    deadline_reason = "data_channel_buffer_deadline";
                } else {
                    deadline_reason = "pacer_token_deadline";
                }
                const std::uint64_t observed_writable_revision = writable_revision;
                const std::uint64_t remaining_us = deadline_us - now_us;
                std::uint64_t wait_us = delay_us == 0
                    || delay_us == std::numeric_limits<std::uint64_t>::max()
                    ? remaining_us
                    : std::min(remaining_us, delay_us);
                const auto wait_begin_us = steady_now_us();
                cv.wait_for(
                    lock,
                    std::chrono::microseconds(wait_us),
                    [&]() {
                        return !running || generation != frame_generation
                            || writable_revision != observed_writable_revision;
                    });
                const auto wake_us = steady_now_us();
                const auto elapsed = wake_us - wait_begin_us;
                if (transport_state.open && in_flight_available && buffered_available) {
                    budget.observe_wait(wait_us, elapsed);
                    telemetry.frame_token_limited = true;
                    telemetry.token_wait_total_us += elapsed;
                    telemetry.timer_lateness_us = budget.lateness_us();
                } else {
                    budget.suspend(wake_us);
                }
                if (tracing) {
                    trace.wait_requested_us += wait_us;
                    trace.wait_elapsed_us += elapsed;
                    const auto overshoot = elapsed > wait_us ? elapsed - wait_us : 0;
                    trace.wait_overshoot_us += overshoot;
                    trace.max_wait_overshoot_us = std::max(trace.max_wait_overshoot_us, overshoot);
                    // Exclusive attribution to the predicate that blocked this wait.
                    if (!transport_state.open) trace.channel_wait_us += elapsed;
                    else if (!in_flight_available) trace.in_flight_wait_us += elapsed;
                    else if (!buffered_available) trace.buffered_wait_us += elapsed;
                    else trace.token_wait_us += elapsed;
                }
            }

            if (!serialize_encoded_video_fragment(
                    view,
                    plan,
                    fragment_index,
                    transport_sequence,
                    &packet,
                    &error)) {
                trace.outcome = 2;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    release_in_flight(wire_bytes);
                    ++telemetry.send_failures;
                }
                publish_event({
                    .type = MediaPacerFrameEventType::kDropped,
                    .frame_id = frame.frame_id,
                    .rate_revision = frame.rate_revision,
                    .target_fps = frame.target_fps,
                    .target_bitrate_kbps = frame.target_bitrate_kbps,
                    .keyframe_refresh_generation = frame.keyframe_refresh_generation,
                    .keyframe = frame.keyframe,
                    .sent_fragments = sent_fragments,
                    .fragment_count = plan.fragment_count,
                    .wire_bytes = plan.wire_bytes,
                    .reason = error,
                });
                require_keyframe(frame, "fragment_serialize_failed", frame_generation);
                return;
            }

            MediaPacerSendResult send_result;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!running || generation != frame_generation) {
                    release_in_flight(packet.size());
                    return;
                }
            }
            const auto send_begin_us = tracing ? steady_now_us() : 0;
            if (tracing && trace.first_send_us == 0) trace.first_send_us = send_begin_us;
            if (send_callback) {
                send_result = send_callback(packet);
            }
            if (tracing) {
                trace.last_send_us = steady_now_us();
                const auto elapsed = trace.last_send_us - send_begin_us;
                trace.send_call_us += elapsed;
                trace.max_send_call_us = std::max(trace.max_send_call_us, elapsed);
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!running || generation != frame_generation) {
                    release_in_flight(packet.size());
                    return;
                }
            }
            if (!send_result.accepted) {
                trace.outcome = 2;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    release_in_flight(packet.size());
                    ++telemetry.send_failures;
                }
                publish_event({
                    .type = MediaPacerFrameEventType::kDropped,
                    .frame_id = frame.frame_id,
                    .rate_revision = frame.rate_revision,
                    .target_fps = frame.target_fps,
                    .target_bitrate_kbps = frame.target_bitrate_kbps,
                    .keyframe_refresh_generation = frame.keyframe_refresh_generation,
                    .keyframe = frame.keyframe,
                    .sent_fragments = sent_fragments,
                    .fragment_count = plan.fragment_count,
                    .wire_bytes = plan.wire_bytes,
                    .reason = "data_channel_send_failed",
                });
                require_keyframe(frame, "data_channel_send_failed", frame_generation);
                return;
            }

            const SentMediaTransportPacket sent_packet{
                .transport_sequence = transport_sequence,
                .frame_id = frame.frame_id,
                .steady_send_us = steady_now_us(),
                .rate_revision = frame.rate_revision,
                .wire_bytes = packet.size(),
                .keyframe = frame.keyframe,
                .first_packet = fragment_index == 0,
                .target_fps = frame.target_fps,
                .target_bitrate_kbps = frame.target_bitrate_kbps,
                .application_limited = fragment_index == 0 && application_limited,
                .probe_generation = probe_generation,
            };
            {
                std::lock_guard lock(mutex);
                telemetry.active_bytes = frame.payload.size() > payload_offset + plan.max_fragment_payload_bytes
                    ? frame.payload.size() - payload_offset - plan.max_fragment_payload_bytes : 0;
                if (probe_generation != 0 && probe_generation == recovery_probe.generation
                    && fragment_index + 1 == plan.fragment_count)
                    telemetry.probe_end_sequence = transport_sequence;
            }
            const auto callback_begin_us = tracing ? steady_now_us() : 0;
            if (packet_sent_callback) {
                packet_sent_callback(sent_packet);
            }
            if (tracing) trace.callback_us += steady_now_us() - callback_begin_us;
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++telemetry.packets_sent;
            }
            ++sent_fragments;
            if (tracing) trace.sent_fragments = sent_fragments;
            sent_wire_bytes += packet.size();
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!running || generation != frame_generation) {
                return;
            }
            ++telemetry.frames_sent;
            active_recovery_frame = false;
            telemetry.next_admission_ms = 0;
            telemetry.rejected_wire_bytes = 0;
            telemetry.budget_retry_attempt = 0;
            if (frame.keyframe
                && frame.recovery_generation == telemetry.recovery_generation) {
                telemetry.keyframe_required = false;
                recovery_notified = false;
            }
        }
        trace.outcome = 1;
        const auto completion_begin_us = tracing ? steady_now_us() : 0;
        publish_event({
            .type = MediaPacerFrameEventType::kSent,
            .frame_id = frame.frame_id,
            .rate_revision = frame.rate_revision,
            .target_fps = frame.target_fps,
            .target_bitrate_kbps = frame.target_bitrate_kbps,
            .keyframe_refresh_generation = frame.keyframe_refresh_generation,
            .keyframe = frame.keyframe,
            .sent_fragments = sent_fragments,
            .fragment_count = plan.fragment_count,
            .wire_bytes = plan.wire_bytes,
            .pacing_bitrate_kbps = pacing_bitrate_kbps,
            .pacing_duration_ms = frame_budget.pacing_duration_ms,
            .deadline_ms = deadline_ms,
            .elapsed_us = steady_now_us() - started_us,
        });
        if (tracing) trace.callback_us += steady_now_us() - completion_begin_us;
    }

    void run() {
        while (true) {
            PacedEncodedVideoFrame frame;
            std::uint64_t frame_generation = 0;
            std::uint64_t frames_sent_before = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                application_limited = pending.empty();
                if (application_limited) budget.suspend(steady_now_us());
                cv.wait(lock, [&]() { return !running || !pending.empty(); });
                if (!running) {
                    return;
                }
                if (application_limited) budget.suspend(steady_now_us());
                while (running && !pending.empty()) {
                    const auto now = steady_now_us();
                    if (now >= next_frame_us) break;
                    cv.wait_for(lock, std::chrono::microseconds(next_frame_us - now));
                }
                if (!running) return;
                if (pending.empty()) continue;
                frame = std::move(pending.front());
                pending.pop_front();
                active_capacity = frame.payload.capacity();
                refresh_queue();
                if (!frame.keyframe && (telemetry.keyframe_required
                        || frame.recovery_generation != telemetry.recovery_generation)) {
                    ++telemetry.dependency_pending_drops;
                    active_capacity = 0;
                    recycle(std::move(frame.payload));
                    lock.unlock();
                    notify_capacity();
                    continue;
                }
                telemetry.active_depth = 1;
                telemetry.frame_token_limited = false;
                telemetry.active_bytes = frame.payload.size();
                frames_sent_before = telemetry.frames_sent;
                active_recovery_frame = frame.recovery_frame;
                frame_generation = generation;
                // Advance a cumulative frame clock. A late wake keeps its
                // fractional remainder, while a long send permits at most one
                // immediate catch-up frame instead of replaying the backlog.
                const auto fps = frame.target_fps;
                const auto slot_us = steady_now_us();
                if (fps == 0) {
                    next_frame_us = 0;
                    frame_cadence_remainder = 0;
                    frame_cadence_fps = 0;
                } else {
                    const auto period = std::max<std::uint64_t>(1, 1000000ULL / fps);
                    if (frame_cadence_fps != fps || next_frame_us == 0
                        || slot_us > next_frame_us + 2 * period) {
                        next_frame_us = slot_us;
                        frame_cadence_remainder = 0;
                        frame_cadence_fps = fps;
                    }
                    const auto numerator = 1000000ULL + frame_cadence_remainder;
                    next_frame_us += numerator / fps;
                    frame_cadence_remainder = numerator % fps;
                }
            }
            cv.notify_all();
            notify_capacity();
            const auto started_us = steady_now_us();
            send_frame(frame, frame_generation);
            {
                std::lock_guard<std::mutex> lock(mutex);
                telemetry.active_depth = 0;
                telemetry.active_bytes = 0;
                active_capacity = 0;
                if (generation == frame_generation && !telemetry.congested
                    && telemetry.frames_sent > frames_sent_before) {
                    const auto service_us = static_cast<double>(steady_now_us() - started_us);
                    if (frame.keyframe) {
                        key_service.add(service_us);
                    } else if (!telemetry.frame_token_limited) {
                        // A token-paced frame measures the selected network
                        // rate, not local scheduling jitter. Learning that wait
                        // as normal service would expand the FIFO precisely
                        // while the sender needs latest-frame backpressure.
                        normal_service.add(service_us);
                    }
                }
                recycle(std::move(frame.payload));
                refresh_queue();
                active_recovery_frame = false;
            }
            cv.notify_all();
            notify_capacity();
        }
    }
};

DesktopMediaSendPacer::DesktopMediaSendPacer()
    : impl_(std::make_unique<Impl>()) {}

DesktopMediaSendPacer::~DesktopMediaSendPacer() {
    stop();
}

bool DesktopMediaSendPacer::start(
    MediaPacerSendCallback send_callback,
    MediaPacerTransportStateCallback transport_state_callback,
    MediaPacerPacketSentCallback packet_sent_callback,
    MediaPacerFrameEventCallback frame_event_callback,
    MediaPacerCapacityCallback capacity_callback,
    MediaFrameTraceRecorder* trace_recorder) {
    if (!send_callback || !transport_state_callback || !packet_sent_callback) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->running) {
        return false;
    }
    impl_->send_callback = std::move(send_callback);
    impl_->transport_state_callback = std::move(transport_state_callback);
    impl_->packet_sent_callback = std::move(packet_sent_callback);
    impl_->frame_event_callback = std::move(frame_event_callback);
    impl_->capacity_callback = std::move(capacity_callback);
    impl_->trace_recorder = trace_recorder;
    impl_->running = true;
    impl_->telemetry.keyframe_required = true;
    impl_->recovery_notified = false;
    impl_->worker = std::thread([impl = impl_.get()]() { impl->run(); });
    return true;
}

void DesktopMediaSendPacer::stop() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running) {
            return;
        }
        impl_->running = false;
        ++impl_->generation;
        impl_->clear_pending();
        impl_->free_payloads.clear();
        worker = std::move(impl_->worker);
    }
    impl_->cv.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
}

void DesktopMediaSendPacer::reset(bool reset_transport_sequence) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    ++impl_->generation;
    impl_->clear_pending();
    impl_->next_frame_us = 0;
    impl_->frame_cadence_remainder = 0;
    impl_->frame_cadence_fps = 0;
    ++impl_->writable_revision;
    impl_->cv.notify_all();
    if (impl_->worker.get_id() != std::this_thread::get_id()) {
        impl_->cv.wait(lock, [&]() { return impl_->telemetry.active_depth == 0; });
    }
    if (reset_transport_sequence) {
        impl_->next_transport_sequence = 0;
        impl_->budget.reset();
        impl_->telemetry.in_flight_bytes = 0;
        impl_->acknowledged_packets = 0;
        impl_->policy = {};
        impl_->normal_service.reset();
        impl_->key_service.reset();
    }
    impl_->telemetry.keyframe_required = true;
    ++impl_->telemetry.recovery_generation;
    impl_->recovery_notified = false;
    impl_->telemetry.next_admission_ms = 0;
    impl_->telemetry.rejected_wire_bytes = 0;
    impl_->telemetry.budget_retry_attempt = 0;
    impl_->recovery_probe = {};
    lock.unlock();
    impl_->notify_capacity();
}

void DesktopMediaSendPacer::update_budget(
    std::uint32_t pacing_bitrate_kbps,
    std::uint32_t smoothed_rtt_ms,
    std::size_t in_flight_bytes,
    const MediaRecoveryProbe* recovery_probe,
    const MediaCongestionDecision* policy) {
    const std::uint64_t now_us = steady_now_us();
    bool admission_changed = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (policy != nullptr) {
            if (policy->decision_revision < impl_->policy.decision_revision) return;
            impl_->policy = *policy;
            impl_->telemetry.congested = policy->pressure != MediaNetworkPressure::kStable;
        } else if (impl_->policy.decision_revision != 0) {
            // Counter-only refreshes cannot overwrite a newer controller rate.
            pacing_bitrate_kbps = impl_->policy.pacing_bitrate_kbps;
        }
        if (impl_->telemetry.pacing_bitrate_kbps != pacing_bitrate_kbps) {
            ++impl_->telemetry.budget_revision;
            impl_->telemetry.next_admission_ms = 0;
            impl_->telemetry.budget_retry_attempt = 0;
            admission_changed = true;
        } else if (impl_->telemetry.smoothed_rtt_ms != smoothed_rtt_ms) {
            // Recalculate an active frame's remaining guard, but an RTT update
            // alone does not authorize another rejected encode attempt.
            ++impl_->telemetry.budget_revision;
        }
        if (recovery_probe != nullptr && recovery_probe->generation >= impl_->recovery_probe.generation) {
            if (recovery_probe->generation != impl_->recovery_probe.generation) {
                impl_->telemetry.probe_wire_bytes = 0;
                impl_->telemetry.probe_end_sequence = 0;
            }
            admission_changed |= recovery_probe->phase != impl_->recovery_probe.phase;
            impl_->recovery_probe = *recovery_probe;
        }
        impl_->telemetry.pacing_bitrate_kbps = pacing_bitrate_kbps;
        impl_->telemetry.smoothed_rtt_ms = smoothed_rtt_ms;
        impl_->telemetry.in_flight_bytes = in_flight_bytes;
        impl_->telemetry.in_flight_limit_bytes = impl_->policy.in_flight_limit_bytes != 0
            ? impl_->policy.in_flight_limit_bytes
            : resolve_pacer_window_bytes(pacing_bitrate_kbps, smoothed_rtt_ms);
        impl_->telemetry.buffered_limit_bytes = impl_->telemetry.in_flight_limit_bytes;
        impl_->budget.set_window_limit(impl_->telemetry.in_flight_limit_bytes);
        impl_->budget.update_rate(pacing_bitrate_kbps, now_us);
        impl_->refresh_queue();
        ++impl_->writable_revision;
    }
    impl_->cv.notify_all();
    if (admission_changed) impl_->notify_capacity();
}

void DesktopMediaSendPacer::update_policy(const MediaCongestionDecision& decision,
    std::uint32_t smoothed_rtt_ms, std::size_t in_flight_bytes) {
    if (decision.pacing_bitrate_kbps != 0)
        update_budget(decision.pacing_bitrate_kbps, smoothed_rtt_ms, in_flight_bytes,
            &decision.recovery_probe, &decision);
}

void DesktopMediaSendPacer::observe_ack_progress(std::uint64_t acknowledged_packets) {
    bool changed = false;
    {
        std::lock_guard lock(impl_->mutex);
        if (acknowledged_packets > impl_->acknowledged_packets) {
            impl_->acknowledged_packets = acknowledged_packets;
            impl_->telemetry.next_admission_ms = 0;
            impl_->telemetry.budget_retry_attempt = 0;
            changed = true;
        }
    }
    if (changed) impl_->notify_capacity();
}

void DesktopMediaSendPacer::notify_writable() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ++impl_->writable_revision;
    }
    impl_->cv.notify_all();
}

bool DesktopMediaSendPacer::can_accept_frame() const {
    return admission().ready();
}

MediaAdmissionResult DesktopMediaSendPacer::admission() const {
    std::lock_guard lock(impl_->mutex);
    impl_->refresh_queue();
    return impl_->admission_locked(steady_now_us() / 1000);
}

MediaAdmissionResult DesktopMediaSendPacer::submit_frame(PacedEncodedVideoFrame frame) {
    auto reservation = reserve_encode();
    if (!reservation) {
        auto result = admission();
        if (result.ready()) result.state = MediaAdmissionState::kWaitCapacity;
        return result;
    }
    return reservation.submit(std::move(frame));
}

MediaAdmissionResult DesktopMediaSendPacer::submit_reserved(PacedEncodedVideoFrame frame, std::uint64_t generation) {
    MediaAdmissionResult result;
    std::unique_lock lock(impl_->mutex);
    if (!impl_->encode_reserved || impl_->reservation_generation != generation)
        return {.state = MediaAdmissionState::kStopped, .detail = "encode reservation expired"};
    impl_->encode_reserved = false;
    impl_->telemetry.reserved_bytes = 0;
    if (!impl_->running || generation != impl_->generation) {
        impl_->recycle(std::move(frame.payload));
        impl_->refresh_queue();
        return {.state = MediaAdmissionState::kStopped, .detail = "capture generation changed during encode"};
    }
    if (frame.frame_id == 0 || frame.rate_revision == 0 || frame.codec == 0
        || frame.width == 0 || frame.height == 0 || frame.target_fps == 0
        || frame.target_bitrate_kbps == 0 || frame.payload.empty()
        || frame.payload.capacity() > transport_detail::kMaximumEncodedFrameBytes) {
        lock.unlock();
        impl_->require_keyframe(frame, "invalid_reserved_frame", generation);
        return {.state = MediaAdmissionState::kInvalidFrame,
                .detail = "encoded frame metadata or payload resource limit invalid"};
    }
    if (!frame.keyframe && (impl_->telemetry.keyframe_required
        || impl_->reservation_recovery_generation != impl_->telemetry.recovery_generation)) {
        result.state = MediaAdmissionState::kWaitRecovery;
        result.detail = "media pacer requires a keyframe";
        impl_->recycle(std::move(frame.payload));
        impl_->refresh_queue();
        lock.unlock();
        impl_->notify_capacity();
        return result;
    }
    frame.recovery_generation = impl_->telemetry.recovery_generation;
    frame.recovery_frame = frame.keyframe && (frame.recovery_frame
        || frame.keyframe_refresh_generation != 0 || impl_->telemetry.keyframe_required);
    frame.enqueued_us = steady_now_us();
    if (frame.trace.enabled) frame.trace.enqueued_us = frame.enqueued_us;
    impl_->telemetry.target_fps = frame.target_fps;
    impl_->pending.push_back(std::move(frame));
    impl_->refresh_queue();
    result.state = MediaAdmissionState::kReady;
    lock.unlock();
    impl_->cv.notify_all();
    return result;
}

DesktopMediaSendPacer::EncodeReservation DesktopMediaSendPacer::reserve_encode() {
    std::lock_guard lock(impl_->mutex);
    impl_->refresh_queue();
    EncodeReservation reservation;
    if (!impl_->admission_locked(steady_now_us() / 1000).ready()) return reservation;
    if (!impl_->free_payloads.empty()) {
        reservation.payload = std::move(impl_->free_payloads.back());
        impl_->free_payloads.pop_back();
    }
    impl_->encode_reserved = true;
    impl_->telemetry.reserved_bytes = transport_detail::kMaximumEncodedFrameBytes;
    impl_->reservation_generation = impl_->generation;
    impl_->reservation_recovery_generation = impl_->telemetry.recovery_generation;
    reservation.owner_ = this;
    reservation.generation_ = impl_->generation;
    return reservation;
}

void DesktopMediaSendPacer::cancel_reservation(std::uint64_t generation, std::vector<std::uint8_t> payload) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->encode_reserved && generation == impl_->reservation_generation) {
            impl_->encode_reserved = false;
            impl_->telemetry.reserved_bytes = 0;
            impl_->recycle(std::move(payload));
            impl_->refresh_queue();
        }
    }
    impl_->notify_capacity();
}

DesktopMediaSendPacer::EncodeReservation::EncodeReservation(EncodeReservation&& other) noexcept
    : payload(std::move(other.payload)), owner_(std::exchange(other.owner_, nullptr)), generation_(other.generation_) {}
DesktopMediaSendPacer::EncodeReservation& DesktopMediaSendPacer::EncodeReservation::operator=(EncodeReservation&& other) noexcept {
    if (this != &other) {
        release();
        payload = std::move(other.payload);
        owner_ = std::exchange(other.owner_, nullptr);
        generation_ = other.generation_;
    }
    return *this;
}
DesktopMediaSendPacer::EncodeReservation::~EncodeReservation() { release(); }
std::size_t DesktopMediaSendPacer::EncodeReservation::payload_limit_bytes() const {
    return transport_detail::kMaximumEncodedFrameBytes;
}
void DesktopMediaSendPacer::EncodeReservation::release() {
    if (auto* owner = std::exchange(owner_, nullptr)) owner->cancel_reservation(generation_, std::move(payload));
}
MediaAdmissionResult DesktopMediaSendPacer::EncodeReservation::submit(PacedEncodedVideoFrame frame) {
    auto* owner = std::exchange(owner_, nullptr);
    return owner ? owner->submit_reserved(std::move(frame), generation_)
        : MediaAdmissionResult{.state = MediaAdmissionState::kStopped, .detail = "no encode reservation"};
}

bool DesktopMediaSendPacer::submit(PacedEncodedVideoFrame frame, std::string* error_detail) {
    const auto result = submit_frame(std::move(frame));
    if (error_detail) *error_detail = result.detail;
    return result.ready();
}

MediaPacerTelemetry DesktopMediaSendPacer::telemetry() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->refresh_queue();
    auto snapshot = impl_->telemetry;
    if (!impl_->pending.empty()) snapshot.oldest_pending_us = steady_now_us() - impl_->pending.front().enqueued_us;
    return snapshot;
}

MediaSendDemand MediaPacerTelemetry::demand() const {
    return {.pending_bytes = pending_bytes + active_bytes,
        .target_fps = target_fps, .token_limited = frame_token_limited && active_bytes != 0,
        .resource_limited = resource_limited, .probe_end_sequence = probe_end_sequence};
}

}  // namespace redclaw::net
