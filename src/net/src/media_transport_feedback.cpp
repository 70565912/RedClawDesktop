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
using transport_detail::resolve_transport_in_flight_expiry_us;
constexpr std::uint64_t kTransportFeedbackIntervalUs = 100000;
constexpr std::size_t kTransportFeedbackPacketTrigger = 32;
constexpr std::size_t kTransportFeedbackPacketLimit = 64;
constexpr std::uint64_t kTransportEstimateWindowUs = 500000;
}  // namespace

bool MediaTransportFeedbackRecorder::record(
    std::uint64_t transport_sequence,
    std::uint64_t receiver_steady_us,
    std::uint64_t observed_rate_revision) {
    if (transport_sequence == 0 || receiver_steady_us == 0
        || observed_rate_revision == 0
        || transport_sequence <= last_transport_sequence_) {
        return false;
    }
    if (!pending_.empty() && observed_rate_revision_ != observed_rate_revision) {
        next_feedback_due_us_ = receiver_steady_us;
        return false;
    }
    last_transport_sequence_ = transport_sequence;
    observed_rate_revision_ = observed_rate_revision;
    pending_.push_back({transport_sequence, receiver_steady_us});
    if (next_feedback_due_us_ == 0) {
        next_feedback_due_us_ = receiver_steady_us + kTransportFeedbackIntervalUs;
    }
    return true;
}

bool MediaTransportFeedbackRecorder::feedback_due(
    std::uint64_t receiver_steady_us) const {
    return !pending_.empty()
        && (pending_.size() >= kTransportFeedbackPacketTrigger
            || (next_feedback_due_us_ != 0
                && receiver_steady_us >= next_feedback_due_us_));
}

std::optional<MediaTransportFeedbackBatch> MediaTransportFeedbackRecorder::take_feedback(
    std::uint64_t receiver_steady_us) {
    if (!feedback_due(receiver_steady_us)) {
        return std::nullopt;
    }
    MediaTransportFeedbackBatch feedback;
    feedback.feedback_id = next_feedback_id_++;
    feedback.observed_rate_revision = observed_rate_revision_;
    const std::size_t count = std::min(pending_.size(), kTransportFeedbackPacketLimit);
    feedback.arrivals.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        feedback.arrivals.push_back(pending_.front());
        pending_.pop_front();
    }
    next_feedback_due_us_ = pending_.empty()
        ? 0
        : receiver_steady_us + kTransportFeedbackIntervalUs;
    return feedback;
}

void MediaTransportFeedbackRecorder::reset() {
    next_feedback_id_ = 1;
    last_transport_sequence_ = 0;
    next_feedback_due_us_ = 0;
    observed_rate_revision_ = 0;
    pending_.clear();
}

std::size_t MediaTransportFeedbackRecorder::pending_arrivals() const noexcept {
    return pending_.size();
}

struct MediaTransportEstimator::Impl {
    struct AckSample {
        std::uint64_t receiver_steady_us = 0;
        std::size_t wire_bytes = 0;
    };
    struct OutcomeSample {
        std::uint64_t receiver_steady_us = 0;
        bool received = false;
    };

    explicit Impl(std::size_t capacity)
        : sent_capacity(std::max<std::size_t>(1, capacity)) {}

    mutable std::mutex mutex;
    std::size_t sent_capacity = 0;
    std::deque<SentMediaTransportPacket> sent;
    std::deque<AckSample> acknowledged_window;
    std::deque<OutcomeSample> outcome_window;
    std::uint64_t last_sent_sequence = 0;
    std::uint64_t last_sent_frame_id = 0;
    std::uint64_t last_sent_frame_us = 0;
    std::uint64_t sent_frame_interval_ms = 0;
    std::uint64_t last_feedback_id = 0;
    std::uint64_t last_current_feedback_id = 0;
    std::uint64_t last_processed_sequence = 0;
    std::uint64_t last_feedback_host_us = 0;
    std::uint64_t last_feedback_rate_revision = 0;
    std::size_t in_flight_bytes = 0;
    std::uint64_t acknowledged_packets = 0;
    std::uint64_t lost_packets = 0;
    std::uint64_t expired_in_flight_packets = 0;
    std::uint64_t expired_in_flight_bytes = 0;
    std::uint64_t ignored_feedback = 0;
    bool anchor_ready = false;
    std::uint64_t anchor_send_us = 0;
    std::uint64_t anchor_arrival_us = 0;
    double smoothed_relative_transit_us = 0.0;
    double minimum_smoothed_relative_transit_us = 0.0;
    std::uint32_t acknowledged_bitrate_kbps = 0;
    std::uint32_t loss_per_mille = 0;
    std::uint32_t queue_delay_ms = 0;

    void expire_in_flight(std::uint64_t host_steady_us, std::uint32_t smoothed_rtt_ms) {
        const std::uint64_t timeout_us = resolve_transport_in_flight_expiry_us(smoothed_rtt_ms);
        while (!sent.empty()
               && host_steady_us >= sent.front().steady_send_us
               && host_steady_us - sent.front().steady_send_us >= timeout_us) {
            const auto expired = sent.front();
            sent.pop_front();
            in_flight_bytes = in_flight_bytes >= expired.wire_bytes
                ? in_flight_bytes - expired.wire_bytes
                : 0;
            ++expired_in_flight_packets;
            expired_in_flight_bytes += expired.wire_bytes;
        }
    }

    void prune(std::uint64_t receiver_steady_us) {
        const std::uint64_t cutoff = receiver_steady_us > kTransportEstimateWindowUs
            ? receiver_steady_us - kTransportEstimateWindowUs
            : 0;
        while (!acknowledged_window.empty()
               && acknowledged_window.front().receiver_steady_us < cutoff) {
            acknowledged_window.pop_front();
        }
        while (!outcome_window.empty()
               && outcome_window.front().receiver_steady_us < cutoff) {
            outcome_window.pop_front();
        }

        std::uint64_t acknowledged_bytes = 0;
        for (const auto& sample : acknowledged_window) {
            acknowledged_bytes += sample.wire_bytes;
        }
        if (acknowledged_window.size() >= 2) {
            const std::uint64_t elapsed_us =
                acknowledged_window.back().receiver_steady_us
                - acknowledged_window.front().receiver_steady_us;
            acknowledged_bitrate_kbps = elapsed_us == 0
                ? 0
                : static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    acknowledged_bytes * 8000ULL / elapsed_us,
                    std::numeric_limits<std::uint32_t>::max()));
        } else {
            acknowledged_bitrate_kbps = 0;
        }

        std::uint64_t received = 0;
        std::uint64_t lost = 0;
        for (const auto& sample : outcome_window) {
            if (sample.received) {
                ++received;
            } else {
                ++lost;
            }
        }
        const std::uint64_t total = received + lost;
        loss_per_mille = total == 0
            ? 0
            : static_cast<std::uint32_t>((lost * 1000ULL) / total);
    }
};

MediaTransportEstimator::MediaTransportEstimator(std::size_t sent_capacity)
    : impl_(std::make_unique<Impl>(sent_capacity)) {}

MediaTransportEstimator::~MediaTransportEstimator() = default;

bool MediaTransportEstimator::record_sent(const SentMediaTransportPacket& packet) {
    if (packet.transport_sequence == 0 || packet.frame_id == 0
        || packet.steady_send_us == 0 || packet.rate_revision == 0
        || packet.wire_bytes == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (packet.transport_sequence <= impl_->last_sent_sequence) {
        return false;
    }
    if (impl_->sent.size() == impl_->sent_capacity) {
        const auto evicted_bytes = impl_->sent.front().wire_bytes;
        impl_->in_flight_bytes -= evicted_bytes;
        impl_->sent.pop_front();
        ++impl_->expired_in_flight_packets;
        impl_->expired_in_flight_bytes += evicted_bytes;
    }
    impl_->sent.push_back(packet);
    if (packet.frame_id != impl_->last_sent_frame_id) {
        if (impl_->last_sent_frame_us != 0 && packet.steady_send_us >= impl_->last_sent_frame_us) {
            impl_->sent_frame_interval_ms = std::min<std::uint64_t>(
                1667, (packet.steady_send_us - impl_->last_sent_frame_us) / 1000);
        }
        impl_->last_sent_frame_id = packet.frame_id;
        impl_->last_sent_frame_us = packet.steady_send_us;
    }
    impl_->last_sent_sequence = packet.transport_sequence;
    impl_->in_flight_bytes += packet.wire_bytes;
    return true;
}

bool MediaTransportEstimator::apply_feedback(
    const MediaTransportFeedbackBatch& feedback,
    std::uint64_t host_steady_us,
    std::uint64_t current_rate_revision) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (feedback.feedback_id == 0
        || feedback.feedback_id <= impl_->last_feedback_id
        || feedback.observed_rate_revision == 0
        || feedback.arrivals.empty()) {
        ++impl_->ignored_feedback;
        return false;
    }
    std::uint64_t previous_sequence = impl_->last_processed_sequence;
    std::uint64_t previous_arrival_us = 0;
    for (const auto& arrival : feedback.arrivals) {
        if (arrival.transport_sequence <= previous_sequence
            || arrival.receiver_steady_us == 0
            || (previous_arrival_us != 0
                && arrival.receiver_steady_us < previous_arrival_us)) {
            ++impl_->ignored_feedback;
            return false;
        }
        previous_sequence = arrival.transport_sequence;
        previous_arrival_us = arrival.receiver_steady_us;
    }

    const bool revision_matches =
        feedback.observed_rate_revision == current_rate_revision;

    for (const auto& arrival : feedback.arrivals) {
        bool blocked_by_newer_revision = false;
        while (!impl_->sent.empty()
               && impl_->sent.front().transport_sequence <= arrival.transport_sequence) {
            const SentMediaTransportPacket sent = impl_->sent.front();
            if (sent.rate_revision > feedback.observed_rate_revision) {
                blocked_by_newer_revision = true;
                break;
            }
            impl_->sent.pop_front();
            impl_->in_flight_bytes -= sent.wire_bytes;
            const bool received = sent.transport_sequence == arrival.transport_sequence;
            if (revision_matches) {
                impl_->outcome_window.push_back({arrival.receiver_steady_us, received});
            }
            if (!received) {
                ++impl_->lost_packets;
                continue;
            }
            ++impl_->acknowledged_packets;
            if (!revision_matches) {
                continue;
            }
            impl_->acknowledged_window.push_back({arrival.receiver_steady_us, sent.wire_bytes});
            if (!impl_->anchor_ready) {
                impl_->anchor_ready = true;
                impl_->anchor_send_us = sent.steady_send_us;
                impl_->anchor_arrival_us = arrival.receiver_steady_us;
                impl_->smoothed_relative_transit_us = 0.0;
                impl_->minimum_smoothed_relative_transit_us = 0.0;
            } else {
                const auto send_delta = static_cast<std::int64_t>(
                    sent.steady_send_us - impl_->anchor_send_us);
                const auto arrival_delta = static_cast<std::int64_t>(
                    arrival.receiver_steady_us - impl_->anchor_arrival_us);
                const double relative_transit_us = static_cast<double>(
                    arrival_delta - send_delta);
                impl_->smoothed_relative_transit_us =
                    impl_->smoothed_relative_transit_us * 0.9
                    + relative_transit_us * 0.1;
                impl_->minimum_smoothed_relative_transit_us = std::min(
                    impl_->minimum_smoothed_relative_transit_us,
                    impl_->smoothed_relative_transit_us);
                const double queue_us = std::max(
                    0.0,
                    impl_->smoothed_relative_transit_us
                        - impl_->minimum_smoothed_relative_transit_us);
                impl_->queue_delay_ms = static_cast<std::uint32_t>(
                    std::min<double>(queue_us / 1000.0, 120000.0));
            }
        }
        if (!blocked_by_newer_revision) {
            impl_->last_processed_sequence = std::max(
                impl_->last_processed_sequence,
                arrival.transport_sequence);
        }
        if (revision_matches) {
            impl_->prune(arrival.receiver_steady_us);
        }
    }
    impl_->last_feedback_id = feedback.feedback_id;
    if (!revision_matches) {
        // Old-revision acknowledgements remain useful for retiring payload-free
        // sent metadata and bounding the pacer in-flight window. They must not,
        // however, refresh or mutate the current revision's rate estimate.
        ++impl_->ignored_feedback;
        return false;
    }
    impl_->last_feedback_rate_revision = current_rate_revision;
    impl_->last_feedback_host_us = host_steady_us;
    impl_->last_current_feedback_id = feedback.feedback_id;
    return true;
}

MediaTransportEstimate MediaTransportEstimator::snapshot(
    std::uint64_t host_steady_us,
    std::uint32_t smoothed_rtt_ms) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->expire_in_flight(host_steady_us, smoothed_rtt_ms);
    MediaTransportEstimate result;
    result.feedback_sample_id = impl_->last_current_feedback_id;
    result.rate_revision = impl_->last_feedback_rate_revision;
    result.acknowledged_bitrate_kbps = impl_->acknowledged_bitrate_kbps;
    result.loss_per_mille = impl_->loss_per_mille;
    result.queue_delay_ms = impl_->queue_delay_ms;
    result.in_flight_bytes = impl_->in_flight_bytes;
    result.acknowledged_packets = impl_->acknowledged_packets;
    result.lost_packets = impl_->lost_packets;
    result.expired_in_flight_packets = impl_->expired_in_flight_packets;
    result.expired_in_flight_bytes = impl_->expired_in_flight_bytes;
    result.ignored_feedback = impl_->ignored_feedback;
    if (impl_->last_feedback_host_us != 0 && host_steady_us >= impl_->last_feedback_host_us) {
        result.feedback_age_ms = (host_steady_us - impl_->last_feedback_host_us) / 1000ULL;
        const std::uint64_t fresh_limit_ms = std::min<std::uint64_t>(5000,
            std::max({1000ULL, impl_->sent_frame_interval_ms * 3ULL,
                static_cast<std::uint64_t>(smoothed_rtt_ms) * 4ULL}));
        result.feedback_fresh = result.feedback_age_ms <= fresh_limit_ms;
    }
    return result;
}

void MediaTransportEstimator::reset() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->sent.clear();
    impl_->acknowledged_window.clear();
    impl_->outcome_window.clear();
    impl_->last_sent_sequence = 0;
    impl_->last_sent_frame_id = 0;
    impl_->last_sent_frame_us = 0;
    impl_->sent_frame_interval_ms = 0;
    impl_->last_feedback_id = 0;
    impl_->last_current_feedback_id = 0;
    impl_->last_processed_sequence = 0;
    impl_->last_feedback_host_us = 0;
    impl_->last_feedback_rate_revision = 0;
    impl_->in_flight_bytes = 0;
    impl_->acknowledged_packets = 0;
    impl_->lost_packets = 0;
    impl_->expired_in_flight_packets = 0;
    impl_->expired_in_flight_bytes = 0;
    impl_->ignored_feedback = 0;
    impl_->anchor_ready = false;
    impl_->anchor_send_us = 0;
    impl_->anchor_arrival_us = 0;
    impl_->smoothed_relative_transit_us = 0.0;
    impl_->minimum_smoothed_relative_transit_us = 0.0;
    impl_->acknowledged_bitrate_kbps = 0;
    impl_->loss_per_mille = 0;
    impl_->queue_delay_ms = 0;
}

}  // namespace redclaw::net
