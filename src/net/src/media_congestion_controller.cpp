#include "redclaw/net/video_frame_transport.h"
#include "media_transport_limits.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace redclaw::net {
namespace {
std::uint32_t rate_kbps(double value) {
    return static_cast<std::uint32_t>(std::clamp(value, 1.0,
        static_cast<double>(std::numeric_limits<std::uint32_t>::max())));
}
constexpr std::size_t kPacketBytes = 16 * 1024;
}

MediaCongestionDecision MediaCongestionController::update(const MediaCongestionSample& sample) {
    std::lock_guard lock(mutex_);
    latest_ = sample;
    return update_locked(latest_);
}

MediaCongestionDecision MediaCongestionController::on_feedback(
    const MediaTransportEstimate& estimate, const MediaSendDemand& demand, std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    latest_.now_steady_ms = now_ms;
    latest_.transport = estimate;
    // A media ACK is not a new observation of the periodic native-send or
    // receiver-damage signal. Do not replay that edge at every ACK cadence.
    latest_.local_backpressure = false;
    const auto receiver_period = latest_.demand.receiver_frame_period_us;
    latest_.demand = demand;
    latest_.demand.receiver_frame_period_us = receiver_period;
    return update_locked(latest_);
}

MediaCongestionDecision MediaCongestionController::update_locked(const MediaCongestionSample& sample) {
    auto& d = decision_;
    ++d.decision_revision;
    d.probe = d.backoff = d.reduce_fps = d.isolated_loss = false;
    if (pacing_bitrate_kbps_ == 0) {
        if (sample.encoder_target_bitrate_kbps == 0) return d;
        // A bootstrap seed, never a measured capacity or a probing ceiling.
        pacing_bitrate_kbps_ = sample.encoder_target_bitrate_kbps;
        bootstrap_rate_kbps_ = pacing_bitrate_kbps_;
        confirmed_rate_kbps_ = pacing_bitrate_kbps_;
    }
    const auto& t = sample.transport;
    const bool new_feedback = t.feedback_fresh && t.feedback_sample_id != 0
        && t.feedback_sample_id > last_transport_feedback_sample_id_;
    const bool new_rtt = sample.rtt_fresh && sample.rtt_sample_id != 0
        && sample.rtt_sample_id > last_rtt_sample_id_;
    if (new_feedback) last_transport_feedback_sample_id_ = t.feedback_sample_id;
    if (new_rtt) {
        last_rtt_sample_id_ = sample.rtt_sample_id;
    }

    const std::uint64_t packet_us = (kPacketBytes * 8000ULL + pacing_bitrate_kbps_ - 1)
        / pacing_bitrate_kbps_;
    const std::uint64_t horizon = std::max<std::uint64_t>({packet_us, t.feedback_round_trip_us,
        t.feedback_interval_us + t.feedback_jitter_us,
        static_cast<std::uint64_t>(sample.smoothed_rtt_ms) * 1000ULL, 1ULL});
    const auto frame_us = sample.demand.target_fps == 0 ? packet_us
        : std::max<std::uint64_t>(1, 1000000ULL / sample.demand.target_fps);
    const auto noise_us = static_cast<std::uint64_t>(queue_noise_.quantile(0.95));
    if (queue_allowance_us_ == 0) queue_allowance_us_ = std::max(frame_us, packet_us);
    const auto allowance = queue_allowance_us_;
    const auto media_queue_us = t.feedback_fresh
        ? static_cast<std::uint64_t>(t.queue_delay_ms) * 1000ULL : 0;
    const auto rtt_queue_us = sample.rtt_fresh
        ? static_cast<std::uint64_t>(sample.rtt_queue_delay_ms) * 1000ULL : 0;
    const auto queue_us = std::max(media_queue_us, rtt_queue_us);
    const bool usable_rate = t.feedback_fresh && t.delivery_rate_valid && !t.application_limited;
    if (new_feedback && usable_rate) delivery_samples_.add(t.delivery_bitrate_kbps);
    const auto variation = std::max(0.0,
        delivery_samples_.quantile(0.95) - delivery_samples_.quantile(0.5));
    if (new_feedback) {
        consecutive_loss_windows_ = t.loss_per_mille != 0
            ? std::min<std::uint32_t>(3, consecutive_loss_windows_ + 1) : 0;
    }
    const bool sustained_loss = consecutive_loss_windows_ >= 3 && usable_rate
        && t.delivery_bitrate_kbps + variation < confirmed_rate_kbps_;
    const bool pressure = sample.local_backpressure || queue_us > allowance || sustained_loss;
    const bool severe = sample.local_backpressure || queue_us > allowance * 2;
    if (new_feedback) {
        media_queue_pressure_rounds_ = usable_rate && media_queue_us > allowance
            ? std::min<std::uint32_t>(2, media_queue_pressure_rounds_ + 1) : 0;
    }
    if (new_rtt) rtt_pressure_rounds_ = rtt_queue_us > allowance
        ? std::min<std::uint32_t>(2, rtt_pressure_rounds_ + 1) : 0;
    const bool delivery_pressure = usable_rate
        && t.delivery_bitrate_kbps + variation < confirmed_rate_kbps_;
    const bool confirmed_media_pressure = media_queue_pressure_rounds_ >= 2
        && delivery_pressure;
    d.isolated_loss = new_feedback && t.loss_per_mille != 0 && !pressure;

    // One authority for ordinary and recovery probes. Only a complete tagged
    // media train confirms capacity; a ping or an arbitrary ACK cannot.
    bool probe_cancelled_for_pressure = false;
    if (recovery_probe_.phase == MediaRecoveryProbePhase::kProbing) {
        const bool completed = new_feedback && sample.demand.probe_end_sequence != 0
            && t.latest_acknowledged_sequence >= sample.demand.probe_end_sequence
            && t.probe_generation == recovery_probe_.generation;
        const auto timeout_us = std::min<std::uint64_t>(
            kMediaPacerRecoveryFrameDeadlineMs * 1000, horizon * 3 + t.feedback_jitter_us);
        const bool expired = sample.now_steady_ms >= recovery_probe_started_ms_
            && (sample.now_steady_ms - recovery_probe_started_ms_) * 1000 >= timeout_us;
        if (completed && t.probe_rate_valid && !pressure && t.loss_per_mille == 0
            && t.probe_delivery_bitrate_kbps > recovery_probe_original_rate_) {
            confirmed_rate_kbps_ = std::min(pacing_bitrate_kbps_, t.probe_delivery_bitrate_kbps);
            pacing_bitrate_kbps_ = confirmed_rate_kbps_;
            recovery_probe_.phase = MediaRecoveryProbePhase::kConfirmed;
            d.reason = "media_probe_confirmed";
        } else if (completed || expired || pressure || !sample.media_channel_open) {
            pacing_bitrate_kbps_ = recovery_probe_original_rate_;
            recovery_probe_.phase = MediaRecoveryProbePhase::kCancelled;
            probe_cancelled_for_pressure = pressure;
            d.reason = completed ? "media_probe_no_capacity_gain" : "media_probe_cancelled";
        }
        if (recovery_probe_.phase != MediaRecoveryProbePhase::kProbing)
            next_probe_ms_ = sample.now_steady_ms + (horizon + t.feedback_jitter_us + 999) / 1000;
    }

    // Receiver arrival timestamps can expose forward-path queuing, but one
    // aggregated or clock-adjusting feedback batch is not enough to reduce the
    // path rate. Require consecutive usable delivery samples, then apply at
    // most one drain decision until the measured pressure has cleared.
    const bool new_pressure_evidence = sample.local_backpressure
        || (new_feedback && (confirmed_media_pressure || sustained_loss))
        || (new_rtt && rtt_pressure_rounds_ >= 2);
    if (!pressure) {
        pressure_backoff_latched_ = false;
    }
    if (pressure && new_pressure_evidence && !pressure_backoff_latched_
        && (!probe_cancelled_for_pressure || sample.local_backpressure || sustained_loss)
        && (last_backoff_sample_ms_ == 0
            || sample.now_steady_ms >= last_backoff_sample_ms_ + (horizon + 999) / 1000)) {
        // A single low delivery batch may be a keyframe burst or ACK gap.
        // Drain from the robust recent delivery floor; a real path reduction
        // moves that floor after sustained samples, while an outlier cannot
        // collapse the sender by an order of magnitude.
        const auto robust_delivery = delivery_samples_.size() < 3
            ? static_cast<double>(t.delivery_bitrate_kbps)
            : delivery_samples_.quantile(0.5);
        const double delivered = usable_rate
            ? std::min<double>(pacing_bitrate_kbps_,
                std::max<double>(t.delivery_bitrate_kbps, robust_delivery))
            : pacing_bitrate_kbps_;
        // Drain measured excess queue instead of applying a fixed percentage.
        const double drain_us = static_cast<double>(std::max(queue_us, allowance));
        pacing_bitrate_kbps_ = rate_kbps(delivered * static_cast<double>(horizon)
            / (static_cast<double>(horizon) + drain_us));
        confirmed_rate_kbps_ = pacing_bitrate_kbps_;
        last_backoff_sample_ms_ = sample.now_steady_ms;
        next_probe_ms_ = sample.now_steady_ms + (horizon + queue_us + 999) / 1000;
        ++backoff_count_;
        pressure_backoff_latched_ = true;
        d.backoff = true;
        d.reduce_fps = !sample.demand.resource_limited;
        d.reason = "measured_congestion_drain";
    }
    d.pressure = pressure ? (severe ? MediaNetworkPressure::kSevere : MediaNetworkPressure::kMild)
        : MediaNetworkPressure::kStable;
    d.reduce_fps = pressure && !sample.demand.resource_limited
        && (sample.local_backpressure || sustained_loss || confirmed_media_pressure
            || (new_rtt && rtt_pressure_rounds_ >= 2));
    // Never normalize sustained congestion into a larger acceptable queue.
    if (new_feedback && !pressure && t.loss_per_mille == 0
        && queue_us <= std::max(frame_us, packet_us)) {
        queue_noise_.add(static_cast<double>(queue_us));
        // A backoff increases packet serialization time. It must not increase
        // tolerance while the very same network queue is still present.
        queue_allowance_us_ = std::max(frame_us, packet_us) + noise_us;
    }

    const bool recovery_needed = sample.recovery_budget_blocked && sample.rtt_fresh
        && t.in_flight_bytes == 0 && sample.buffered_amount == 0;
    const bool media_demand = sample.demand.pending_bytes > kPacketBytes
        && sample.demand.token_limited && !sample.demand.resource_limited;
    if (!pressure && sample.media_channel_open && !sample.local_backpressure
        && recovery_probe_.phase != MediaRecoveryProbePhase::kProbing
        && sample.now_steady_ms >= next_probe_ms_
        && ((new_feedback && usable_rate && media_demand) || recovery_needed)) {
        // A collapsed token rate must not define its own recovery clock. Use
        // actual feedback/ping cadence and a bounded bootstrap packet flight;
        // this grants a probe, not confirmation of that bootstrap capacity.
        const auto bootstrap_rate = std::max(1U, usable_rate ? t.delivery_bitrate_kbps
            : bootstrap_rate_kbps_);
        const auto probe_horizon = recovery_needed ? std::max<std::uint64_t>({
            kPacketBytes * 8000ULL / bootstrap_rate,
            static_cast<std::uint64_t>(sample.smoothed_rtt_ms) * 1000,
            t.feedback_fresh ? t.feedback_interval_us + t.feedback_jitter_us : 0, 1}) : horizon;
        const auto headroom_us = allowance > queue_us ? allowance - queue_us : 0;
        const double confidence = confirmed_rate_kbps_
            / (static_cast<double>(confirmed_rate_kbps_) + variation);
        const auto extra_bytes = std::clamp<std::size_t>(static_cast<std::size_t>(
            confirmed_rate_kbps_ * static_cast<double>(headroom_us) / 8000.0 * confidence),
            kPacketBytes, transport_detail::kMaximumPacerWindowBytes);
        const auto demand_bytes = recovery_needed ? extra_bytes + kPacketBytes : sample.demand.pending_bytes;
        const auto probe_bytes = std::min<std::size_t>(demand_bytes,
            transport_detail::kMaximumPacerWindowBytes);
        const auto desired = std::max<double>(confirmed_rate_kbps_,
            static_cast<double>(demand_bytes) * 8000.0 / static_cast<double>(probe_horizon));
        const auto candidate = rate_kbps(std::min(desired, confirmed_rate_kbps_
            + extra_bytes * 8000.0 / static_cast<double>(probe_horizon)));
        if (candidate > confirmed_rate_kbps_ && probe_bytes > kPacketBytes) {
            recovery_probe_original_rate_ = pacing_bitrate_kbps_;
            pacing_bitrate_kbps_ = candidate;
            ++recovery_probe_.generation;
            recovery_probe_.phase = MediaRecoveryProbePhase::kProbing;
            recovery_probe_.wire_budget_bytes = probe_bytes;
            recovery_probe_.baseline_rate_kbps = recovery_probe_original_rate_;
            recovery_probe_.recovery = recovery_needed;
            recovery_probe_started_ms_ = sample.now_steady_ms;
            ++probe_count_;
            d.probe = true;
            d.reason = "measured_headroom_probe";
        }
    }

    d.pacing_bitrate_kbps = pacing_bitrate_kbps_;
    d.feedback_horizon_us = horizon;
    d.queue_allowance_us = allowance;
    d.receiver_frame_period_us = sample.demand.receiver_frame_period_us;
    d.delivery_rate_valid = usable_rate;
    const auto feedback_bytes = static_cast<double>(pacing_bitrate_kbps_)
        * static_cast<double>(horizon) / 8000.0;
    d.in_flight_limit_bytes = static_cast<std::size_t>(std::clamp(
        feedback_bytes + static_cast<double>(t.acknowledged_batch_bytes),
        static_cast<double>(kPacketBytes),
        static_cast<double>(transport_detail::kMaximumPacerWindowBytes)));
    if (t.feedback_sample_id == 0) d.in_flight_limit_bytes = kPacketBytes;
    d.probe_count = probe_count_;
    d.backoff_count = backoff_count_;
    d.recovery_probe = recovery_probe_;
    if (sample.demand.resource_limited) d.reason = "sender_resource_limited";
    else if (!t.feedback_fresh) d.reason = "feedback_uncertain";
    else if (t.application_limited && !pressure && !d.probe) d.reason = "application_limited";
    return d;
}

void MediaCongestionController::reset() {
    std::lock_guard lock(mutex_);
    latest_ = {};
    const auto revision = decision_.decision_revision;
    decision_ = {};
    decision_.decision_revision = revision + 1;
    delivery_samples_.reset();
    queue_noise_.reset();
    next_probe_ms_ = queue_allowance_us_ = 0;
    confirmed_rate_kbps_ = bootstrap_rate_kbps_ = pacing_bitrate_kbps_ = 0;
    consecutive_loss_windows_ = rtt_pressure_rounds_ = media_queue_pressure_rounds_ = 0;
    probe_count_ = backoff_count_ = 0;
    last_transport_feedback_sample_id_ = last_rtt_sample_id_ = 0;
    recovery_probe_ = {};
    recovery_probe_started_ms_ = last_backoff_sample_ms_ = 0;
    recovery_probe_original_rate_ = 0;
    pressure_backoff_latched_ = false;
}
}  // namespace redclaw::net
