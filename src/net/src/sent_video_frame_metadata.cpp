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

SentVideoFrameMetadataRing::SentVideoFrameMetadataRing(std::size_t capacity)
    : capacity_(capacity) {}

bool SentVideoFrameMetadataRing::record(const SentVideoFrameMetadata& metadata) {
    if (capacity_ == 0
        || metadata.frame_id == 0
        || metadata.steady_send_time_ms == 0
        || metadata.rate_revision == 0
        || metadata.fps == 0
        || metadata.bitrate_kbps == 0) {
        return false;
    }
    if (!entries_.empty() && metadata.frame_id <= entries_.back().frame_id) {
        return false;
    }
    if (entries_.size() == capacity_) {
        entries_.pop_front();
    }
    entries_.push_back(metadata);
    return true;
}

bool SentVideoFrameMetadataRing::finish(
    std::uint64_t frame_id, SentVideoFrameState state, std::uint16_t fragments) {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->frame_id == frame_id) {
            it->state = state;
            it->sent_fragments = fragments;
            return true;
        }
        if (it->frame_id < frame_id) break;
    }
    return false;
}

std::optional<SentVideoFrameMetadata> SentVideoFrameMetadataRing::find(
    std::uint64_t frame_id) const {
    for (auto iter = entries_.rbegin(); iter != entries_.rend(); ++iter) {
        if (iter->frame_id == frame_id) {
            return *iter;
        }
        if (iter->frame_id < frame_id) {
            break;
        }
    }
    return std::nullopt;
}

void SentVideoFrameMetadataRing::clear() {
    entries_.clear();
}

std::size_t SentVideoFrameMetadataRing::size() const noexcept {
    return entries_.size();
}

std::size_t SentVideoFrameMetadataRing::capacity() const noexcept {
    return capacity_;
}

ReceiverFeedbackAssessment assess_receiver_feedback(
    const std::optional<SentVideoFrameMetadata>& sent_frame,
    std::uint64_t now_steady_ms,
    std::uint32_t smoothed_rtt_ms,
    std::uint64_t observed_rate_revision,
    std::uint64_t current_rate_revision) {
    ReceiverFeedbackAssessment result;
    const std::uint64_t doubled_rtt_ms = static_cast<std::uint64_t>(smoothed_rtt_ms) * 2ULL;
    result.fresh_limit_ms = (std::max)(250ULL, doubled_rtt_ms);
    result.expired_limit_ms = (std::max)(1000ULL, doubled_rtt_ms * 2ULL);
    if (!sent_frame.has_value()) {
        return result;
    }
    if (sent_frame->rate_revision == 0
        || observed_rate_revision != sent_frame->rate_revision
        || current_rate_revision != sent_frame->rate_revision) {
        result.freshness = ReceiverFeedbackFreshness::kStaleRevision;
        return result;
    }
    if (now_steady_ms < sent_frame->steady_send_time_ms) {
        result.freshness = ReceiverFeedbackFreshness::kExpired;
        return result;
    }
    result.feedback_age_ms = now_steady_ms - sent_frame->steady_send_time_ms;
    if (result.feedback_age_ms <= result.fresh_limit_ms) {
        result.freshness = ReceiverFeedbackFreshness::kFresh;
    } else if (result.feedback_age_ms <= result.expired_limit_ms) {
        result.freshness = ReceiverFeedbackFreshness::kDelayed;
    } else {
        result.freshness = ReceiverFeedbackFreshness::kExpired;
    }
    return result;
}

}  // namespace redclaw::net
