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

namespace redclaw::net {
namespace {

constexpr std::uint32_t kMinimumPacingBitrateKbps = 400;
}  // namespace

MediaCongestionDecision MediaCongestionController::update(
    const MediaCongestionSample& sample) {
    MediaCongestionDecision decision;
    const std::uint32_t encoder_target = std::min<std::uint32_t>(
        sample.encoder_target_bitrate_kbps,
        20000);
    if (encoder_target == 0) {
        return decision;
    }
    if (pacing_bitrate_kbps_ == 0) {
        pacing_bitrate_kbps_ = encoder_target;
    }
    // The encoder target is the useful probe ceiling, not a command to drain an
    // already healthy pacer at the encoder's average output rate.  Keeping the
    // current pacing ceiling when the encoder target falls preserves room for a
    // multi-fragment IDR burst; actual traffic is still bounded by encoder output,
    // the token bucket, in-flight budget, and DataChannel high-water mark.
    pacing_bitrate_kbps_ = std::min<std::uint32_t>(pacing_bitrate_kbps_, 20000);

    const bool new_transport_sample = sample.transport.feedback_fresh
        && sample.transport.feedback_sample_id != 0
        && sample.transport.feedback_sample_id > last_transport_feedback_sample_id_;
    if (new_transport_sample) {
        last_transport_feedback_sample_id_ = sample.transport.feedback_sample_id;
    }
    const std::uint32_t transport_queue_delay_ms =
        new_transport_sample ? sample.transport.queue_delay_ms : 0;
    const std::uint32_t transport_loss_per_mille =
        new_transport_sample ? sample.transport.loss_per_mille : 0;
    const bool new_rtt_sample = sample.rtt_fresh && sample.rtt_sample_id != 0
        && sample.rtt_sample_id > last_rtt_sample_id_;
    if (new_rtt_sample) {
        last_rtt_sample_id_ = sample.rtt_sample_id;
        healthy_rtt_samples_ = sample.rtt_queue_delay_ms < 40
            ? std::min<std::uint32_t>(2, healthy_rtt_samples_ + 1) : 0;
    }
    if (recovery_probe_.phase == MediaRecoveryProbePhase::kProbing) {
        const bool ack_progress = new_transport_sample
            && sample.transport.acknowledged_packets > recovery_probe_ack_base_
            && sample.transport.queue_delay_ms < 40 && sample.transport.loss_per_mille == 0
            && !sample.local_backpressure;
        const auto probe_timeout_ms = std::clamp<std::uint64_t>(
            2ULL * sample.smoothed_rtt_ms + 500, 1500, 5000);
        if (ack_progress) {
            recovery_probe_.phase = MediaRecoveryProbePhase::kConfirmed;
        } else if (sample.local_backpressure || !sample.media_channel_open
                   || sample.now_steady_ms >= recovery_probe_started_ms_ + probe_timeout_ms) {
            pacing_bitrate_kbps_ = recovery_probe_original_rate_;
            recovery_probe_.phase = MediaRecoveryProbePhase::kCancelled;
        }
    }
    const std::uint32_t queue_delay_ms = std::max(
        new_rtt_sample ? sample.rtt_queue_delay_ms : 0U,
        transport_queue_delay_ms);
    const bool severe_rtt = new_rtt_sample && sample.rtt_queue_delay_ms >= 200;
    const bool repeated_severe_rtt = severe_rtt && last_severe_rtt_ms_ != 0
        && sample.now_steady_ms >= last_severe_rtt_ms_
        && sample.now_steady_ms - last_severe_rtt_ms_ <= 3000;
    if (new_rtt_sample) {
        last_severe_rtt_ms_ = severe_rtt ? sample.now_steady_ms : 0;
    }
    const bool severe_pressure = sample.local_backpressure || queue_delay_ms >= 200;
    const bool loss_window = transport_loss_per_mille >= 20;
    const bool mild_pressure = queue_delay_ms >= 120
        || (loss_window && queue_delay_ms >= 40);
    const bool isolated_loss = transport_loss_per_mille > 0
        && transport_loss_per_mille < 20
        && queue_delay_ms < 40;

    if (loss_window) {
        consecutive_loss_windows_ = std::min<std::uint32_t>(
            3,
            consecutive_loss_windows_ + 1);
    } else if (new_transport_sample) {
        consecutive_loss_windows_ = 0;
    }
    const bool sustained_loss = consecutive_loss_windows_ >= 3;
    if (severe_pressure || mild_pressure || sustained_loss) {
        const std::uint32_t scale = severe_pressure ? 70U : 85U;
        pacing_bitrate_kbps_ = std::max<std::uint32_t>(
            kMinimumPacingBitrateKbps,
            static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(pacing_bitrate_kbps_) * scale / 100ULL));
        stable_since_ms_ = 0;
        if (sustained_loss) {
            consecutive_loss_windows_ = 0;
        }
        ++backoff_count_;
        decision.pressure = severe_pressure
            ? MediaNetworkPressure::kSevere
            : MediaNetworkPressure::kMild;
        decision.backoff = true;
        decision.reduce_fps = sample.local_backpressure || transport_queue_delay_ms >= 120
            || sustained_loss || repeated_severe_rtt
            || (mild_pressure && !severe_pressure);
    } else {
        const bool stable = sample.transport.feedback_fresh && sample.rtt_fresh
            && sample.rtt_queue_delay_ms < 40 && sample.transport.queue_delay_ms < 40
            && sample.transport.loss_per_mille == 0
            && !sample.local_backpressure;
        if (stable) {
            if (stable_since_ms_ == 0) {
                stable_since_ms_ = sample.now_steady_ms;
            } else if (new_transport_sample && sample.now_steady_ms >= stable_since_ms_ + 2000
                       && pacing_bitrate_kbps_ < encoder_target) {
                const std::uint32_t percent_probe = static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(pacing_bitrate_kbps_) * 108ULL + 99ULL)
                    / 100ULL);
                pacing_bitrate_kbps_ = std::min(
                    encoder_target,
                    std::max(pacing_bitrate_kbps_ + 100U, percent_probe));
                stable_since_ms_ = sample.now_steady_ms;
                ++probe_count_;
                decision.probe = true;
            }
        } else {
            stable_since_ms_ = 0;
        }
    }
    if (sample.recovery_budget_blocked && !sample.transport.feedback_fresh
        && recovery_probe_.phase != MediaRecoveryProbePhase::kProbing
        && sample.media_channel_open && sample.buffered_amount == 0
        && sample.transport.in_flight_bytes == 0 && !sample.local_backpressure
        && sample.rtt_fresh && healthy_rtt_samples_ >= 2
        && (recovery_probe_started_ms_ == 0
            || sample.now_steady_ms >= recovery_probe_started_ms_ + 10000)
        && pacing_bitrate_kbps_ < encoder_target) {
        recovery_probe_original_rate_ = pacing_bitrate_kbps_;
        pacing_bitrate_kbps_ = std::min(encoder_target,
            pacing_bitrate_kbps_ + std::max<std::uint32_t>(100, pacing_bitrate_kbps_ / 4));
        ++recovery_probe_.generation;
        recovery_probe_.phase = MediaRecoveryProbePhase::kProbing;
        recovery_probe_started_ms_ = sample.now_steady_ms;
        recovery_probe_ack_base_ = sample.transport.acknowledged_packets;
        ++probe_count_;
        decision.probe = true;
    }
    decision.recovery_probe = recovery_probe_;
    decision.pacing_bitrate_kbps = pacing_bitrate_kbps_;
    decision.isolated_loss = isolated_loss;
    decision.probe_count = probe_count_;
    decision.backoff_count = backoff_count_;
    return decision;
}

void MediaCongestionController::reset() {
    pacing_bitrate_kbps_ = 0;
    consecutive_loss_windows_ = 0;
    stable_since_ms_ = 0;
    probe_count_ = 0;
    backoff_count_ = 0;
    last_transport_feedback_sample_id_ = 0;
    last_rtt_sample_id_ = 0;
    last_severe_rtt_ms_ = 0;
    recovery_probe_ = {};
    recovery_probe_started_ms_ = 0;
    recovery_probe_ack_base_ = 0;
    recovery_probe_original_rate_ = 0;
    healthy_rtt_samples_ = 0;
}

}  // namespace redclaw::net
