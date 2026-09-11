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

}  // namespace

void MediaPacingBudget::update_rate(
    std::uint32_t pacing_bitrate_kbps,
    std::uint64_t now_steady_us) {
    const bool initialize = last_refill_us_ == 0;
    if (!initialize) {
        refill(now_steady_us);
    }
    pacing_bitrate_kbps_ = pacing_bitrate_kbps;
    burst_bytes_ = std::clamp<double>(
        static_cast<double>(pacing_bitrate_kbps_) * 1000.0 / 8.0 * 0.020,
        static_cast<double>(kFragmentHeaderBytes + 16 * 1024),
        64.0 * 1024.0);
    if (initialize) {
        last_refill_us_ = now_steady_us;
        tokens_bytes_ = burst_bytes_;
    } else {
        tokens_bytes_ = std::min(tokens_bytes_, burst_bytes_);
    }
}

void MediaPacingBudget::refill(std::uint64_t now_steady_us) {
    if (last_refill_us_ == 0) {
        last_refill_us_ = now_steady_us;
        return;
    }
    if (now_steady_us <= last_refill_us_ || pacing_bitrate_kbps_ == 0) {
        return;
    }
    const std::uint64_t elapsed_us = now_steady_us - last_refill_us_;
    tokens_bytes_ = std::min(
        burst_bytes_,
        tokens_bytes_ + static_cast<double>(pacing_bitrate_kbps_)
            * 1000.0 / 8.0 * static_cast<double>(elapsed_us) / 1000000.0);
    last_refill_us_ = now_steady_us;
}

std::uint64_t MediaPacingBudget::delay_until_available_us(
    std::size_t wire_bytes,
    std::uint64_t now_steady_us) {
    refill(now_steady_us);
    if (tokens_bytes_ >= static_cast<double>(wire_bytes)) {
        return 0;
    }
    if (pacing_bitrate_kbps_ == 0) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const double required_bytes = static_cast<double>(wire_bytes) - tokens_bytes_;
    const double bytes_per_us = static_cast<double>(pacing_bitrate_kbps_)
        * 1000.0 / 8.0 / 1000000.0;
    return static_cast<std::uint64_t>(std::ceil(required_bytes / bytes_per_us));
}

bool MediaPacingBudget::consume(
    std::size_t wire_bytes,
    std::uint64_t now_steady_us) {
    refill(now_steady_us);
    if (tokens_bytes_ < static_cast<double>(wire_bytes)) {
        return false;
    }
    tokens_bytes_ -= static_cast<double>(wire_bytes);
    return true;
}

void MediaPacingBudget::reset() {
    pacing_bitrate_kbps_ = 0;
    last_refill_us_ = 0;
    tokens_bytes_ = 0.0;
    burst_bytes_ = 0.0;
}

MediaPacerFrameBudget resolve_media_pacer_frame_budget(
    std::size_t wire_bytes,
    std::uint32_t pacing_bitrate_kbps,
    std::uint32_t smoothed_rtt_ms,
    bool recovery_frame) {
    MediaPacerFrameBudget budget;
    if (wire_bytes == 0 || pacing_bitrate_kbps == 0) {
        return budget;
    }

    const std::uint64_t bytes_per_second =
        static_cast<std::uint64_t>(pacing_bitrate_kbps) * 1000ULL / 8ULL;
    if (bytes_per_second == 0) {
        return budget;
    }

    budget.guard_duration_ms = std::clamp<std::uint64_t>(
        static_cast<std::uint64_t>(smoothed_rtt_ms) * 2ULL + 100ULL,
        250ULL,
        2000ULL);
    const std::uint64_t maximum_deadline_ms = recovery_frame
        ? kMediaPacerRecoveryFrameDeadlineMs : kMediaPacerMaximumFrameDeadlineMs;
    const std::uint64_t maximum_pacing_ms = maximum_deadline_ms - budget.guard_duration_ms;
    const std::uint64_t maximum_wire_bytes =
        bytes_per_second * maximum_pacing_ms / 1000ULL;
    const std::uint64_t wire_bytes_u64 = static_cast<std::uint64_t>(wire_bytes);
    if (wire_bytes_u64 > maximum_wire_bytes) {
        budget.pacing_duration_ms = maximum_pacing_ms + 1ULL;
        budget.deadline_ms = maximum_deadline_ms;
        return budget;
    }
    budget.pacing_duration_ms =
        (wire_bytes_u64 * 1000ULL + bytes_per_second - 1ULL) / bytes_per_second;
    const std::uint64_t required_ms = budget.pacing_duration_ms
        > std::numeric_limits<std::uint64_t>::max() - budget.guard_duration_ms
        ? std::numeric_limits<std::uint64_t>::max()
        : budget.pacing_duration_ms + budget.guard_duration_ms;
    budget.deadline_ms = std::min(
        required_ms,
        maximum_deadline_ms);
    budget.feasible = required_ms <= maximum_deadline_ms;
    return budget;
}

}  // namespace redclaw::net
