#include "redclaw/render/audio_jitter_buffer.h"

#include <algorithm>

namespace redclaw::render {
namespace {

constexpr std::uint64_t kSilenceGapUs = 100000;
constexpr std::uint32_t kMaxPlcGap = 2;
constexpr std::size_t kMaxQueuedFrames = 8;

}  // namespace

AudioJitterBuffer::AudioJitterBuffer(std::uint32_t prebuffer_frames)
    : prebuffer_frames_(std::max<std::uint32_t>(1, prebuffer_frames)) {}

void AudioJitterBuffer::reset() {
    frames_.clear();
    next_sequence_ = 0;
    primed_ = false;
    played_ = false;
    have_arrival_ = false;
    first_arrival_us_ = 0;
    last_arrival_us_ = 0;
    last_timestamp_us_ = 0;
}

void AudioJitterBuffer::push(AudioJitterFrame frame, std::uint64_t now_us) {
    if (frame.sequence == 0 || frame.payload.empty()) {
        return;
    }
    if (primed_ && frame.sequence < next_sequence_) {
        return;
    }
    last_arrival_us_ = now_us;
    if (!have_arrival_) {
        first_arrival_us_ = now_us;
        have_arrival_ = true;
    }
    frames_[frame.sequence] = std::move(frame);
    bool dropped_oldest = false;
    while (frames_.size() > kMaxQueuedFrames) {
        frames_.erase(frames_.begin());
        dropped_oldest = true;
    }
    // A normal gap stays put so pull() can conceal one or two missing frames.
    // Only an overflow drop may skip the cursor; otherwise pull() treats the
    // hole as a stream break and stops output.
    if (dropped_oldest && primed_ && !frames_.empty() && frames_.begin()->first > next_sequence_) {
        next_sequence_ = frames_.begin()->first;
    }
}

AudioJitterPull AudioJitterBuffer::pull(std::uint64_t now_us) {
    AudioJitterPull result;
    if (frames_.empty() && !primed_) {
        return result;
    }
    if (!primed_) {
        const bool enough = frames_.size() >= prebuffer_frames_;
        const bool waited = have_arrival_ && now_us >= first_arrival_us_ + 60000;
        if (!enough && !waited) {
            return result;
        }
        primed_ = true;
        next_sequence_ = frames_.begin()->first;
    }
    const auto exact = frames_.find(next_sequence_);
    if (exact != frames_.end()) {
        result.action = AudioJitterAction::kPacket;
        result.frame = std::move(exact->second);
        frames_.erase(exact);
        last_timestamp_us_ = result.frame.timestamp_us;
        played_ = true;
        ++next_sequence_;
        return result;
    }
    const auto later = std::find_if(frames_.begin(), frames_.end(), [&](const auto& item) {
        return item.first > next_sequence_;
    });
    if (later == frames_.end()) {
        if (played_ && have_arrival_ && now_us >= last_arrival_us_ + kSilenceGapUs) {
            result.action = AudioJitterAction::kSilence;
            reset();
        }
        return result;
    }
    const std::uint32_t gap = later->first - next_sequence_;
    const bool recent = have_arrival_ && now_us < last_arrival_us_ + kSilenceGapUs;
    if (gap <= kMaxPlcGap && recent) {
        result.action = AudioJitterAction::kPlc;
        ++next_sequence_;
        return result;
    }
    frames_.erase(frames_.begin(), later);
    primed_ = false;
    played_ = false;
    have_arrival_ = true;
    next_sequence_ = 0;
    first_arrival_us_ = now_us;
    result.action = AudioJitterAction::kSilence;
    return result;
}

}  // namespace redclaw::render
