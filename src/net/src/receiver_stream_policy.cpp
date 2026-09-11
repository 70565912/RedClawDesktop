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


}  // namespace

ReceiverAssemblyQuality evaluate_receiver_assembly_quality(
    const ReceiverAssemblyQualityWindow& window) {
    ReceiverAssemblyQuality quality;
    const std::uint64_t attempted_frames =
        window.completed_frames > std::numeric_limits<std::uint64_t>::max() - window.dropped_frames
            ? std::numeric_limits<std::uint64_t>::max()
            : window.completed_frames + window.dropped_frames;
    if (attempted_frames > 0) {
        const long double scaled_loss = std::min<long double>(
            1000.0L,
            (static_cast<long double>(window.dropped_frames) * 1000.0L)
                / static_cast<long double>(attempted_frames));
        quality.loss_per_mille = static_cast<std::uint32_t>(scaled_loss);
    }

    if (window.dropped_keyframes > 0
        || window.reassembly_timeouts > 0
        || quality.loss_per_mille >= 250) {
        quality.pressure = ReceiverAssemblyPressure::kSevere;
        quality.fps_scale_percent = 25;
    } else if (quality.loss_per_mille >= 100) {
        quality.pressure = ReceiverAssemblyPressure::kDegraded;
        quality.fps_scale_percent = 50;
    } else if (window.dropped_frames > 0) {
        quality.pressure = ReceiverAssemblyPressure::kMild;
        quality.fps_scale_percent = 80;
    }
    return quality;
}

ReceiverDecodeCapacityDecision ReceiverDecodeCapacityController::update(
    const ReceiverDecodeCapacitySample& sample) {
    ReceiverDecodeCapacityDecision decision;
    decision.target_fps = std::max<std::uint32_t>(1, sample.current_target_fps);
    decision.decrease_total = decrease_total_;
    decision.increase_total = increase_total_;

    if (!sample.revision_consistent || !sample.source_active
        || sample.rate_revision != rate_revision_ || sample.source_revision != source_revision_) {
        pressure_windows_ = 0;
        stable_windows_ = 0;
        window_ms_ = reassembled_ = decoded_ = healthy_ms_ = previous_backlog_ = 0;
        rate_revision_ = sample.rate_revision;
        source_revision_ = sample.source_revision;
    }
    if (!sample.revision_consistent || !sample.source_active || sample.window_ms == 0
        || sample.window_ms > 10000 || sample.reassembled_frames > 100000
        || sample.decoded_frames > 100000) {
        return decision;
    }
    window_ms_ += sample.window_ms;
    reassembled_ += sample.reassembled_frames;
    decoded_ += sample.decoded_frames;
    if (sample.hard_pressure) {
        healthy_ms_ = 0;
    } else if (sample.network_stable) {
        healthy_ms_ += sample.window_ms;
    }
    // GUI decode counters trail runtime reassembly. Judge a mature window,
    // allowing one frame still in the bounded pipeline, not a 0/1 tick.
    if (window_ms_ < 5000 || reassembled_ < 5) {
        if (window_ms_ >= 30000) {
            window_ms_ = reassembled_ = decoded_ = healthy_ms_ = 0;
        }
        return decision;
    }
    decision.valid = true;
    const long double observed_decode_fps =
        static_cast<long double>(decoded_) * 1000.0L / window_ms_;
    decision.observed_decode_fps = static_cast<std::uint32_t>(std::min<long double>(
        observed_decode_fps,
        static_cast<long double>(std::numeric_limits<std::uint32_t>::max())));
    const long double decode_ratio =
        static_cast<long double>(decoded_ + 1) / static_cast<long double>(reassembled_);
    const auto backlog = sample.pending_frames.value_or(
        reassembled_ > decoded_ ? reassembled_ - decoded_ : 0);
    // Reaching the latest complete frame proves older gaps were superseded,
    // not a growing decoder queue. A growing sequence lag remains pressure.
    decision.pressure = decode_ratio < 0.90L && backlog > 1
        && (!sample.pending_frames || backlog > previous_backlog_);
    decision.stable = decode_ratio >= 0.95L || (sample.pending_frames && backlog <= 1);

    if (decision.pressure) {
        pressure_windows_ = std::min<std::uint32_t>(pressure_windows_ + 1, 2);
        stable_windows_ = 0;
        healthy_ms_ = 0;
        if (pressure_windows_ >= 2 && decision.target_fps > 1) {
            const std::uint32_t capacity_fps = std::max<std::uint32_t>(1, decision.observed_decode_fps);
            decision.target_fps = std::max<std::uint32_t>(
                1,
                std::min(decision.target_fps - 1, capacity_fps));
            decision.target_changed = decision.target_fps != sample.current_target_fps;
            if (decision.target_changed) {
                ++decrease_total_;
                pressure_windows_ = 0;
            }
        }
    } else if (decision.stable && sample.network_stable) {
        pressure_windows_ = 0;
        stable_windows_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(5, healthy_ms_ / 1000));
        const std::uint32_t maximum_target_fps = std::max<std::uint32_t>(
            1,
            sample.maximum_target_fps);
        if (stable_windows_ >= 5 && decision.target_fps < maximum_target_fps) {
            ++decision.target_fps;
            decision.target_changed = true;
            ++increase_total_;
            stable_windows_ = 0;
            healthy_ms_ = 0;
        }
    } else if (!decision.stable || sample.hard_pressure) {
        pressure_windows_ = 0;
        stable_windows_ = 0;
        healthy_ms_ = 0;
    }
    if (decision.pressure || decision.target_changed || window_ms_ >= 30000) {
        previous_backlog_ = backlog;
        window_ms_ = reassembled_ = decoded_ = 0;
    }

    decision.pressure_windows = pressure_windows_;
    decision.stable_windows = stable_windows_;
    decision.decrease_total = decrease_total_;
    decision.increase_total = increase_total_;
    return decision;
}

void ReceiverDecodeCapacityController::reset() {
    pressure_windows_ = 0;
    stable_windows_ = 0;
    decrease_total_ = 0;
    increase_total_ = 0;
    window_ms_ = reassembled_ = decoded_ = healthy_ms_ = 0;
    rate_revision_ = source_revision_ = previous_backlog_ = 0;
}

std::uint32_t select_stream_target_fps(
    std::uint32_t current_fps, bool encoder_pressure,
    const MediaCongestionDecision& network,
    const ReceiverDecodeCapacityDecision& capacity) {
    auto target = std::max(1U, current_fps);
    if (encoder_pressure) {
        target = std::min(target, saturating_reduce_stream_target_fps(current_fps, 2));
    }
    if (network.reduce_fps) {
        target = std::min(target, saturating_reduce_stream_target_fps(current_fps,
            network.pressure == MediaNetworkPressure::kSevere ? 3 : 1));
    }
    if (capacity.target_changed) {
        if (capacity.target_fps < current_fps) {
            target = std::min(target, capacity.target_fps);
        } else if (target == current_fps && !network.backoff && !encoder_pressure) {
            target = capacity.target_fps;
        }
    }
    return target;
}

std::uint32_t saturating_reduce_stream_target_fps(
    std::uint32_t current_fps,
    std::uint32_t decrement) {
    if (current_fps <= 1) {
        return 1;
    }
    const std::uint32_t available_reduction = current_fps - 1;
    return current_fps - std::min(decrement, available_reduction);
}

StreamRttSignal evaluate_stream_rtt_signal(
    std::uint32_t latest_rtt_ms,
    std::uint32_t minimum_rtt_ms,
    std::uint32_t relief_queue_delay_ms,
    std::uint32_t high_queue_delay_ms,
    std::uint32_t severe_queue_delay_ms) {
    StreamRttSignal signal;
    if (latest_rtt_ms == 0
        || minimum_rtt_ms == 0
        || relief_queue_delay_ms > high_queue_delay_ms
        || high_queue_delay_ms > severe_queue_delay_ms) {
        return signal;
    }

    signal.valid = true;
    signal.baseline_rtt_ms = minimum_rtt_ms;
    signal.queue_delay_ms = latest_rtt_ms > minimum_rtt_ms
        ? latest_rtt_ms - minimum_rtt_ms
        : 0;
    signal.severe_pressure = signal.queue_delay_ms >= severe_queue_delay_ms;
    signal.high_pressure = signal.queue_delay_ms >= high_queue_delay_ms;
    signal.relief = signal.queue_delay_ms < relief_queue_delay_ms;
    return signal;
}

std::uint32_t resolve_available_encode_budget_gap(
    std::uint32_t captured_frames,
    std::uint32_t encoded_frames,
    std::uint32_t target_budget_frames) {
    const std::uint32_t available_budget = std::min(
        captured_frames,
        target_budget_frames);
    return available_budget > encoded_frames
        ? (available_budget - encoded_frames)
        : 0;
}

AvailableEncodeBudgetPressure evaluate_available_encode_budget_pressure(
    std::uint32_t captured_frames,
    std::uint32_t encoded_frames,
    std::uint32_t target_budget_frames) {
    const std::uint32_t available_budget = std::min(
        captured_frames,
        target_budget_frames);
    const std::uint32_t gap = resolve_available_encode_budget_gap(
        captured_frames,
        encoded_frames,
        target_budget_frames);
    if (available_budget == 0 || gap == 0) {
        return AvailableEncodeBudgetPressure::kStable;
    }

    const std::uint32_t severe_threshold = std::max<std::uint32_t>(
        7,
        static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(available_budget) + 4ULL) / 5ULL));
    if (gap >= severe_threshold) {
        return AvailableEncodeBudgetPressure::kSevere;
    }

    const std::uint32_t pressure_threshold = std::max<std::uint32_t>(
        3,
        static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(available_budget) + 9ULL) / 10ULL));
    return gap >= pressure_threshold
        ? AvailableEncodeBudgetPressure::kPressure
        : AvailableEncodeBudgetPressure::kStable;
}

DesktopStreamHealth classify_desktop_stream_health(
    const DesktopStreamHealthSample& sample) {
    if (sample.role == DesktopStreamEndpointRole::kHost) {
        if (sample.transmit_failures > 0) {
            return DesktopStreamHealth::kSendFailures;
        }
        if (sample.transmit_backpressure > 0) {
            return DesktopStreamHealth::kSendBackpressure;
        }
        if (sample.encode_failures > 0 || sample.encode_backpressure) {
            return DesktopStreamHealth::kEncodeBackpressure;
        }
        if (sample.startup_tracking
            && sample.startup_timeout_ms > 0
            && sample.startup_elapsed_ms >= sample.startup_timeout_ms
            && sample.transmitted_frames_total == 0) {
            if (sample.captured_frames_total == 0) {
                return DesktopStreamHealth::kCaptureStartupStalled;
            }
            if (sample.encoded_frames_total == 0) {
                return DesktopStreamHealth::kEncoderStartupStalled;
            }
            return DesktopStreamHealth::kTransmitStartupStalled;
        }
        if (sample.receiver_loss_per_mille > 0) {
            return sample.relay_path
                ? DesktopStreamHealth::kRelayLoss
                : DesktopStreamHealth::kNetworkLoss;
        }
        if (sample.transmitted_frames == 0) {
            return DesktopStreamHealth::kIdle;
        }
    } else {
        if (sample.direct_pipe_write_failures > 0) {
            return DesktopStreamHealth::kDirectPipeWriteFailures;
        }
        if (sample.decode_failures > 0) {
            return DesktopStreamHealth::kDecodeFailures;
        }
        if (sample.render_failures > 0) {
            return DesktopStreamHealth::kRenderFailures;
        }
        if (sample.startup_tracking
            && sample.startup_timeout_ms > 0
            && sample.startup_elapsed_ms >= sample.startup_timeout_ms
            && sample.delivered_frames_total == 0) {
            if (sample.received_frames_total == 0) {
                return DesktopStreamHealth::kReceiveStartupStalled;
            }
            return DesktopStreamHealth::kPlaybackStartupStalled;
        }
        if (sample.direct_pipe_connected) {
            if (sample.received_frames == 0
                && sample.direct_pipe_writer_frames == 0
                && sample.direct_pipe_reader_frames == 0) {
                return DesktopStreamHealth::kIdle;
            }
            const std::uint64_t direct_pipe_progress_gap =
                sample.direct_pipe_writer_frames > sample.direct_pipe_reader_frames
                ? (sample.direct_pipe_writer_frames - sample.direct_pipe_reader_frames)
                : 0;
            if (sample.direct_pipe_backlog >= 2 || direct_pipe_progress_gap >= 3) {
                return DesktopStreamHealth::kPlaybackBacklog;
            }
        } else {
            const std::uint32_t playback_gap = sample.received_frames > sample.rendered_frames
                ? (sample.received_frames - sample.rendered_frames)
                : 0;
            if (playback_gap >= 3) {
                return DesktopStreamHealth::kPlaybackBacklog;
            }
            if (sample.received_frames == 0 && sample.rendered_frames == 0) {
                return DesktopStreamHealth::kIdle;
            }
        }
    }

    if (sample.relay_path) {
        return DesktopStreamHealth::kRelayPath;
    }
    if (sample.direct_nat_path) {
        return DesktopStreamHealth::kDirectNatPath;
    }
    return DesktopStreamHealth::kHealthy;
}

std::string_view desktop_stream_health_name(DesktopStreamHealth health) noexcept {
    switch (health) {
    case DesktopStreamHealth::kIdle:
        return "idle";
    case DesktopStreamHealth::kCaptureStartupStalled:
        return "capture_startup_stalled";
    case DesktopStreamHealth::kEncoderStartupStalled:
        return "encoder_startup_stalled";
    case DesktopStreamHealth::kTransmitStartupStalled:
        return "transmit_startup_stalled";
    case DesktopStreamHealth::kReceiveStartupStalled:
        return "receive_startup_stalled";
    case DesktopStreamHealth::kPlaybackStartupStalled:
        return "playback_startup_stalled";
    case DesktopStreamHealth::kSendFailures:
        return "send_failures";
    case DesktopStreamHealth::kSendBackpressure:
        return "send_backpressure";
    case DesktopStreamHealth::kDecodeFailures:
        return "decode_failures";
    case DesktopStreamHealth::kRenderFailures:
        return "render_failures";
    case DesktopStreamHealth::kEncodeBackpressure:
        return "encode_backpressure";
    case DesktopStreamHealth::kDirectPipeWriteFailures:
        return "direct_pipe_write_failures";
    case DesktopStreamHealth::kNetworkLoss:
        return "network_loss";
    case DesktopStreamHealth::kRelayLoss:
        return "relay_loss";
    case DesktopStreamHealth::kPlaybackBacklog:
        return "playback_backlog";
    case DesktopStreamHealth::kRelayPath:
        return "relay_path";
    case DesktopStreamHealth::kDirectNatPath:
        return "direct_nat_path";
    case DesktopStreamHealth::kHealthy:
        return "healthy";
    }
    return "healthy";
}

}  // namespace redclaw::net
