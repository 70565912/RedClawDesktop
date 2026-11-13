#pragma once
#include <cstdint>

namespace redclaw::helper {
enum class PlaybackPauseReason : std::uint8_t { kVisible, kHidden, kDisplayOff, kOccluded, kRecovery };

// Local notification only. A future negotiated sender hint can consume this
// snapshot; no sender pacing or input authorization is controlled by it.
struct PlaybackStateSnapshot {
    std::uint64_t session = 0;
    std::uint64_t revision = 0;
    bool paused = false;
    PlaybackPauseReason reason = PlaybackPauseReason::kVisible;
    std::uint64_t boundary = 0;
    bool resynchronizing = false;
    bool valid = true;
};

// An in-flight decode or queued callback belongs to the transition at submit.
// Pause followed by resume cannot make an older result current again.
[[nodiscard]] inline bool playback_output_is_current(const PlaybackStateSnapshot& submitted,
    const PlaybackStateSnapshot& current) noexcept {
    return submitted.valid && current.valid && !submitted.paused && !current.paused
        && submitted.session == current.session && submitted.revision == current.revision;
}
}  // namespace redclaw::helper
