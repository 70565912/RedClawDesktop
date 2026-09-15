#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace redclaw::ui {
// Frame IDs belong to a Host session. Local decode generations additionally
// exclude an in-flight frame/telemetry snapshot from before a reset.
class PlaybackFrameProgress {
public:
    std::uint64_t generation = 1;
    std::uint64_t displayable = 0, keyframe = 0, presented = 0, reported = 0;
    bool observe_source(std::string_view epoch) {
        if (epoch.empty() || epoch == source_epoch_) return false;
        const bool changed = !source_epoch_.empty();
        source_epoch_ = epoch;
        return changed;
    }
    void reset_frames(std::uint64_t next_generation) {
        generation = next_generation;
        displayable = keyframe = presented = reported = 0;
    }
    bool accept(std::uint64_t frame_generation, std::uint64_t frame_id, bool is_keyframe) {
        if (frame_generation != generation) return false;
        displayable = std::max(displayable, frame_id);
        if (is_keyframe) keyframe = std::max(keyframe, frame_id);
        return true;
    }
    bool mark_presented(std::uint64_t frame_id) {
        if (frame_id <= presented) return false;
        presented = frame_id;
        return true;
    }
    void observe_transport(std::uint64_t snapshot_generation, std::uint64_t frame_id, std::uint64_t keyframe_id) {
        if (snapshot_generation != generation) return;
        displayable = std::max(displayable, frame_id);
        keyframe = std::max(keyframe, keyframe_id);
    }
private:
    std::string source_epoch_;
};
}
