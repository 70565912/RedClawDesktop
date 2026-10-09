#pragma once
#include "redclaw/capture/capture_recovery.h"
#include <mutex>

namespace redclaw::capture {
enum class CaptureGeometryChange { kRegion, kDisplay };
struct CaptureStreamSnapshot {
    CaptureAvailability availability = CaptureAvailability::kStopped;
    std::uint64_t generation = 0;
    std::uint64_t first_frame_id = 0;
    bool presented = false;
    std::uint64_t input_pause_revision = 0;
    std::uint64_t input_reset_revision = 0;
    bool navigation_pending = false;
};
// Owns the boundary between the capture worker, encoder and presentation ACK.
// Input authorization remains in RemoteInputSession, including explicit re-arm.
class CaptureStreamGate {
public:
    void begin_geometry_change(CaptureGeometryChange change) {
        std::lock_guard lock(mutex_);
        ++state_.input_reset_revision;
        if (change != CaptureGeometryChange::kDisplay) return;
        if (state_.availability != CaptureAvailability::kRunning) return;
        state_.navigation_pending = true;
        navigation_generation_ = state_.generation;
        state_.first_frame_id = 0;
        state_.presented = false;
    }
    bool update(CaptureAvailability availability, std::uint64_t generation) {
        std::lock_guard lock(mutex_);
        const bool invalidate = generation != state_.generation
            || (availability != CaptureAvailability::kRunning && state_.availability == CaptureAvailability::kRunning);
        if (invalidate) {
            state_.first_frame_id = 0; state_.presented = false;
            if (!state_.navigation_pending || availability != CaptureAvailability::kRunning)
                ++state_.input_pause_revision;
        }
        if (availability != CaptureAvailability::kRunning) state_.navigation_pending = false;
        state_.availability = availability; state_.generation = generation;
        return invalidate;
    }
    bool accepts(std::uint64_t generation) const {
        std::lock_guard lock(mutex_);
        return accepts_locked(generation);
    }
    template <typename Submit> bool submit(std::uint64_t generation, Submit&& operation) {
        std::lock_guard lock(mutex_);
        if (!accepts_locked(generation)) { return false; }
        operation();
        return true;
    }
    bool submitted_keyframe(std::uint64_t generation, std::uint64_t frame_id) {
        std::lock_guard lock(mutex_);
        if (!accepts_locked(generation) || state_.first_frame_id != 0) { return false; }
        state_.first_frame_id = frame_id;
        return true;
    }
    bool presented(std::uint64_t frame_id) {
        std::lock_guard lock(mutex_);
        if (state_.availability != CaptureAvailability::kRunning || state_.first_frame_id == 0
            || frame_id < state_.first_frame_id || state_.presented) { return false; }
        state_.presented = true;
        state_.navigation_pending = false;
        return true;
    }
    CaptureStreamSnapshot snapshot() const { std::lock_guard lock(mutex_); return state_; }
private:
    bool accepts_locked(std::uint64_t generation) const {
        return state_.availability == CaptureAvailability::kRunning && state_.generation == generation
            && (!state_.navigation_pending || generation > navigation_generation_);
    }
    mutable std::mutex mutex_;
    CaptureStreamSnapshot state_;
    std::uint64_t navigation_generation_ = 0;
};
}
