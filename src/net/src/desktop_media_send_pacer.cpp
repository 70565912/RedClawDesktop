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
}  // namespace

struct DesktopMediaSendPacer::Impl {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool running = false;
    std::uint64_t generation = 1;
    std::uint64_t writable_revision = 0;
    std::uint64_t next_transport_sequence = 0;
    std::optional<PacedEncodedVideoFrame> pending;
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
    MediaPacerTelemetry telemetry;
    MediaAdmissionResult admission_locked(std::uint64_t now_ms) const {
        MediaAdmissionResult result;
        result.budget_revision = telemetry.budget_revision;
        result.next_check_ms = telemetry.next_admission_ms;
        if (!running) result.state = MediaAdmissionState::kStopped;
        else if (pending) result.state = MediaAdmissionState::kWaitCapacity;
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
            if (pending && !pending->keyframe) {
                pending.reset();
                telemetry.pending_depth = 0;
                ++telemetry.dependency_pending_drops;
            } else if (pending) {
                pending->recovery_generation = telemetry.recovery_generation;
            }
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

    void send_frame(PacedEncodedVideoFrame frame, std::uint64_t frame_generation) {
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
        if (!frame_budget.feasible && probe_generation == 0) {
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
                const std::uint64_t now_us = steady_now_us();
                {
                    std::lock_guard lock(mutex);
                    if (!running || generation != frame_generation) return;
                    if (budget_revision != telemetry.budget_revision) {
                        budget_revision = telemetry.budget_revision;
                        pacing_bitrate_kbps = telemetry.pacing_bitrate_kbps;
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
                    if (probe_generation != 0 && recovery_probe.generation == probe_generation
                        && recovery_probe.phase == MediaRecoveryProbePhase::kCancelled) {
                        deadline_us = now_us;
                        deadline_reason = "recovery_probe_unconfirmed";
                    }
                }
                if (now_us >= deadline_us) {
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
                if (transport_state_callback) {
                    transport_state = transport_state_callback();
                }
                std::unique_lock<std::mutex> lock(mutex);
                if (!running || generation != frame_generation) {
                    return;
                }
                const std::uint64_t delay_us = budget.delay_until_available_us(
                    wire_bytes,
                    now_us);
                const bool in_flight_available = telemetry.in_flight_bytes + wire_bytes
                    <= telemetry.in_flight_limit_bytes;
                const bool buffered_available = transport_state.open
                    && transport_state.buffered_amount + wire_bytes
                        <= telemetry.buffered_limit_bytes;
                const bool probe_available = probe_generation == 0
                    || (recovery_probe.generation == probe_generation
                        && recovery_probe.phase == MediaRecoveryProbePhase::kConfirmed)
                    || telemetry.probe_wire_bytes + wire_bytes <= 64U * 1024U;
                if (delay_us == 0 && in_flight_available && buffered_available && probe_available
                    && budget.consume(wire_bytes, now_us)) {
                    transport_sequence = ++next_transport_sequence;
                    telemetry.in_flight_bytes += wire_bytes;
                    if (probe_generation != 0) telemetry.probe_wire_bytes += wire_bytes;
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
                cv.wait_for(
                    lock,
                    std::chrono::microseconds(wait_us),
                    [&]() {
                        return !running || generation != frame_generation
                            || writable_revision != observed_writable_revision;
                    });
            }

            if (!serialize_encoded_video_fragment(
                    view,
                    plan,
                    fragment_index,
                    transport_sequence,
                    &packet,
                    &error)) {
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
            if (send_callback) {
                send_result = send_callback(packet);
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!running || generation != frame_generation) {
                    release_in_flight(packet.size());
                    return;
                }
            }
            if (!send_result.accepted) {
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
            };
            if (packet_sent_callback) {
                packet_sent_callback(sent_packet);
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++telemetry.packets_sent;
            }
            ++sent_fragments;
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
    }

    void run() {
        while (true) {
            PacedEncodedVideoFrame frame;
            std::uint64_t frame_generation = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&]() { return !running || pending.has_value(); });
                if (!running) {
                    return;
                }
                frame = std::move(*pending);
                pending.reset();
                telemetry.pending_depth = 0;
                if (!frame.keyframe && (telemetry.keyframe_required
                        || frame.recovery_generation != telemetry.recovery_generation)) {
                    ++telemetry.dependency_pending_drops;
                    lock.unlock();
                    notify_capacity();
                    continue;
                }
                telemetry.active_depth = 1;
                active_recovery_frame = frame.recovery_frame;
                frame_generation = generation;
            }
            cv.notify_all();
            notify_capacity();
            send_frame(std::move(frame), frame_generation);
            {
                std::lock_guard<std::mutex> lock(mutex);
                telemetry.active_depth = 0;
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
    MediaPacerCapacityCallback capacity_callback) {
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
        impl_->pending.reset();
        impl_->telemetry.pending_depth = 0;
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
    impl_->pending.reset();
    impl_->telemetry.pending_depth = 0;
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
    const MediaRecoveryProbe* recovery_probe) {
    const std::uint64_t now_us = steady_now_us();
    bool admission_changed = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
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
            }
            admission_changed |= recovery_probe->phase != impl_->recovery_probe.phase;
            impl_->recovery_probe = *recovery_probe;
        }
        impl_->telemetry.pacing_bitrate_kbps = pacing_bitrate_kbps;
        impl_->telemetry.smoothed_rtt_ms = smoothed_rtt_ms;
        impl_->telemetry.in_flight_bytes = in_flight_bytes;
        impl_->telemetry.in_flight_limit_bytes = resolve_pacer_window_bytes(
            pacing_bitrate_kbps,
            smoothed_rtt_ms);
        impl_->telemetry.buffered_limit_bytes = impl_->telemetry.in_flight_limit_bytes;
        impl_->budget.update_rate(pacing_bitrate_kbps, now_us);
        ++impl_->writable_revision;
    }
    impl_->cv.notify_all();
    if (admission_changed) impl_->notify_capacity();
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
    return impl_->admission_locked(steady_now_us() / 1000);
}

MediaAdmissionResult DesktopMediaSendPacer::submit_frame(PacedEncodedVideoFrame frame) {
    if (frame.frame_id == 0 || frame.rate_revision == 0 || frame.codec == 0
        || frame.width == 0 || frame.height == 0 || frame.target_fps == 0
        || frame.target_bitrate_kbps == 0 || frame.payload.empty()) {
        return {.state = MediaAdmissionState::kInvalidFrame,
                .detail = "paced encoded frame metadata is incomplete"};
    }
    MediaAdmissionResult result;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        result = impl_->admission_locked(steady_now_us() / 1000);
        if (!result.ready()) return result;
        if (impl_->telemetry.keyframe_required && !frame.keyframe) {
            result.state = MediaAdmissionState::kWaitRecovery;
            result.detail = "media pacer requires a keyframe";
            return result;
        }
        frame.recovery_generation = impl_->telemetry.recovery_generation;
        frame.recovery_frame = frame.keyframe && (frame.recovery_frame
            || frame.keyframe_refresh_generation != 0 || impl_->telemetry.keyframe_required);
        impl_->pending = std::move(frame);
        impl_->telemetry.pending_depth = 1;
    }
    impl_->cv.notify_all();
    return result;
}

bool DesktopMediaSendPacer::submit(PacedEncodedVideoFrame frame, std::string* error_detail) {
    const auto result = submit_frame(std::move(frame));
    if (error_detail) *error_detail = result.detail;
    return result.ready();
}

MediaPacerTelemetry DesktopMediaSendPacer::telemetry() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->telemetry;
}

}  // namespace redclaw::net
