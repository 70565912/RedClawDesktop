#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace redclaw::net {

std::uint64_t media_trace_now_us();

// Local diagnostics only. Never serialized into media/control messages.
struct MediaFrameTrace {
    bool enabled = false;
    std::uint64_t frame_id = 0, rate_revision = 0, capture_generation = 0, capture_sequence = 0;
    std::uint64_t capture_begin_us = 0, capture_end_us = 0, capture_ready_us = 0;
    std::uint64_t encode_begin_us = 0, encode_end_us = 0, enqueued_us = 0;
    std::uint64_t pacer_begin_us = 0, first_send_us = 0, last_send_us = 0, finish_us = 0;
    std::uint64_t wire_bytes = 0, sent_fragments = 0, fragment_count = 0;
    std::uint64_t token_wait_us = 0, in_flight_wait_us = 0, buffered_wait_us = 0;
    std::uint64_t channel_wait_us = 0, probe_wait_us = 0;
    std::uint64_t wait_requested_us = 0, wait_elapsed_us = 0;
    std::uint64_t wait_overshoot_us = 0, max_wait_overshoot_us = 0;
    std::uint64_t transport_state_us = 0, send_call_us = 0, max_send_call_us = 0, callback_us = 0;
    std::uint32_t width = 0, height = 0, target_fps = 0, pacing_kbps = 0, rtt_ms = 0;
    std::uint32_t outcome = 0; // 1 sent, 2 failed/dropped, 3 cancelled by local lifecycle.
    bool keyframe = false;
};

struct MediaFrameTraceBatch {
    std::uint64_t started_us = 0, ended_us = 0, overflow = 0;
    std::vector<MediaFrameTrace> frames;
};

class MediaFrameTraceRecorder final {
public:
    static constexpr std::size_t kCapacity = 4096;
    // Control-thread calls only. At most 120 seconds, followed by 11 seconds
    // for already admitted recovery frames to complete (their deadline is 10 s).
    bool arm(std::uint64_t now_us, std::uint32_t seconds);
    [[nodiscard]] bool active(std::uint64_t now_us) const;
    void record(const MediaFrameTrace& frame);
    std::optional<MediaFrameTraceBatch> take_completed(std::uint64_t now_us);
private:
    std::atomic<std::uint64_t> deadline_us_{0};
    std::atomic<bool> full_{false};
    std::mutex mutex_;
    MediaFrameTraceBatch batch_;
};

} // namespace redclaw::net
