#include "redclaw/net/media_frame_trace.h"
#include <chrono>

namespace redclaw::net {
std::uint64_t media_trace_now_us() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool MediaFrameTraceRecorder::arm(std::uint64_t now_us, std::uint32_t seconds) {
    if (now_us == 0 || seconds == 0 || seconds > 120) return false;
    std::lock_guard lock(mutex_);
    if (deadline_us_.load() != 0) return false;
    batch_ = {};
    batch_.frames.reserve(kCapacity);
    batch_.started_us = now_us;
    batch_.ended_us = now_us + static_cast<std::uint64_t>(seconds) * 1000000;
    full_.store(false);
    deadline_us_.store(batch_.ended_us);
    return true;
}

bool MediaFrameTraceRecorder::active(std::uint64_t now_us) const {
    const auto deadline = deadline_us_.load(std::memory_order_relaxed);
    return deadline != 0 && now_us < deadline && !full_.load(std::memory_order_relaxed);
}

void MediaFrameTraceRecorder::record(const MediaFrameTrace& frame) {
    if (!frame.enabled) return;
    std::lock_guard lock(mutex_);
    if (deadline_us_.load() == 0 || frame.encode_begin_us < batch_.started_us
        || frame.encode_begin_us >= batch_.ended_us) return;
    if (batch_.frames.size() == kCapacity) {
        ++batch_.overflow;
        full_.store(true);
        return;
    }
    batch_.frames.push_back(frame);
}

std::optional<MediaFrameTraceBatch> MediaFrameTraceRecorder::take_completed(std::uint64_t now_us) {
    const auto deadline = deadline_us_.load();
    if (deadline == 0 || now_us < deadline || now_us - deadline < 11000000) return std::nullopt;
    std::lock_guard lock(mutex_);
    deadline_us_.store(0);
    full_.store(false);
    return std::move(batch_);
}
} // namespace redclaw::net
