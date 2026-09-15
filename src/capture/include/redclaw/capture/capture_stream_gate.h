#pragma once
#include "redclaw/capture/capture_recovery.h"
#include <mutex>

namespace redclaw::capture {
struct CaptureStreamSnapshot {
    CaptureAvailability availability = CaptureAvailability::kStopped;
    std::uint64_t generation = 0;
    std::uint64_t first_frame_id = 0;
    bool presented = false;
    std::uint64_t input_pause_revision = 0;
};
// Owns the boundary between the capture worker, encoder and presentation ACK.
// Input authorization remains in RemoteInputSession, including explicit re-arm.
class CaptureStreamGate {
public:
    bool update(CaptureAvailability availability, std::uint64_t generation) {
        std::lock_guard lock(mutex_);
        const bool invalidate = generation != state_.generation
            || (availability != CaptureAvailability::kRunning && state_.availability == CaptureAvailability::kRunning);
        if (invalidate) { state_.first_frame_id = 0; state_.presented = false; ++state_.input_pause_revision; }
        state_.availability = availability; state_.generation = generation;
        return invalidate;
    }
    bool accepts(std::uint64_t generation) const {
        std::lock_guard lock(mutex_);
        return state_.availability == CaptureAvailability::kRunning && state_.generation == generation;
    }
    template <typename Submit> bool submit(std::uint64_t generation, Submit&& operation) {
        std::lock_guard lock(mutex_);
        if (state_.availability != CaptureAvailability::kRunning || state_.generation != generation) { return false; }
        operation();
        return true;
    }
    bool submitted_keyframe(std::uint64_t generation, std::uint64_t frame_id) {
        std::lock_guard lock(mutex_);
        if (state_.availability != CaptureAvailability::kRunning || state_.generation != generation
            || state_.first_frame_id != 0) { return false; }
        state_.first_frame_id = frame_id;
        return true;
    }
    bool presented(std::uint64_t frame_id) {
        std::lock_guard lock(mutex_);
        if (state_.availability != CaptureAvailability::kRunning || state_.first_frame_id == 0
            || frame_id < state_.first_frame_id || state_.presented) { return false; }
        state_.presented = true;
        return true;
    }
    CaptureStreamSnapshot snapshot() const { std::lock_guard lock(mutex_); return state_; }
private:
    mutable std::mutex mutex_;
    CaptureStreamSnapshot state_;
};
}
