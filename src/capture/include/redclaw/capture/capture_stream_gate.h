#pragma once
#include "redclaw/capture/capture_recovery.h"
#include <mutex>

namespace redclaw::capture {
enum class CaptureGeometryChange { kRegion, kDisplay };
enum class CaptureStreamUpdate { kObservation, kAcquireTimeout };
struct CaptureStreamSnapshot {
    CaptureAvailability availability = CaptureAvailability::kStopped;
    std::uint64_t generation = 0;
    std::uint64_t first_frame_id = 0;
    bool presented = false;
    std::uint64_t input_pause_revision = 0;
    std::uint64_t navigation_timeout_revision = 0;
    std::uint64_t input_reset_revision = 0;
    bool navigation_pending = false;
};
// Owns the boundary between the capture worker, encoder and presentation ACK.
// Input authorization remains in RemoteInputSession, including explicit re-arm.
class CaptureStreamGate {
public:
    static constexpr std::uint64_t kNavigationTimeoutMs = 5000;

    void begin_geometry_change(CaptureGeometryChange change, std::uint64_t now_ms) {
        std::lock_guard lock(mutex_);
        ++state_.input_reset_revision;
        if (change != CaptureGeometryChange::kDisplay) return;
        if (state_.availability != CaptureAvailability::kRunning && !state_.navigation_pending) return;
        state_.navigation_pending = true;
        navigation_generation_ = state_.generation;
        navigation_start_generation_ = 0;
        navigation_deadline_ms_ = now_ms + kNavigationTimeoutMs;
        state_.first_frame_id = 0;
        state_.presented = false;
    }
    void started_capture(std::uint64_t generation) {
        std::lock_guard lock(mutex_);
        if (state_.navigation_pending && navigation_start_generation_ == 0)
            navigation_start_generation_ = generation;
    }
    bool update(CaptureAvailability availability, std::uint64_t generation,
        CaptureStreamUpdate kind = CaptureStreamUpdate::kObservation) {
        std::lock_guard lock(mutex_);
        const bool waiting_for_first_frame = kind == CaptureStreamUpdate::kAcquireTimeout
            && availability == CaptureAvailability::kRecovering
            && generation > navigation_generation_ && generation == navigation_start_generation_;
        const bool planned_generation = generation == navigation_start_generation_;
        const bool lost_navigation = state_.navigation_pending
            && !(planned_generation && availability == CaptureAvailability::kRunning)
            && !waiting_for_first_frame;
        const bool invalidate = generation != state_.generation
            || (availability != CaptureAvailability::kRunning && state_.availability == CaptureAvailability::kRunning);
        if (invalidate || lost_navigation) {
            state_.first_frame_id = 0; state_.presented = false;
            if (!state_.navigation_pending || lost_navigation)
                ++state_.input_pause_revision;
        }
        if (lost_navigation) state_.navigation_pending = false;
        state_.availability = availability; state_.generation = generation;
        return invalidate || lost_navigation;
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
    bool presented(std::uint64_t frame_id, std::uint64_t now_ms) {
        std::lock_guard lock(mutex_);
        expire_navigation_locked(now_ms);
        if (state_.availability != CaptureAvailability::kRunning || state_.first_frame_id == 0
            || frame_id < state_.first_frame_id || state_.presented) { return false; }
        state_.presented = true;
        state_.navigation_pending = false;
        return true;
    }
    CaptureStreamSnapshot snapshot() const { std::lock_guard lock(mutex_); return state_; }
    CaptureStreamSnapshot input_snapshot(std::uint64_t now_ms) {
        std::lock_guard lock(mutex_);
        expire_navigation_locked(now_ms);
        return state_;
    }
private:
    void expire_navigation_locked(std::uint64_t now_ms) {
        if (state_.navigation_pending && now_ms >= navigation_deadline_ms_) {
            state_.navigation_pending = false;
            state_.navigation_timeout_revision = ++state_.input_pause_revision;
        }
    }
    bool accepts_locked(std::uint64_t generation) const {
        return state_.availability == CaptureAvailability::kRunning && state_.generation == generation
            && generation > navigation_generation_;
    }
    mutable std::mutex mutex_;
    CaptureStreamSnapshot state_;
    std::uint64_t navigation_generation_ = 0;
    std::uint64_t navigation_start_generation_ = 0;
    std::uint64_t navigation_deadline_ms_ = 0;
};
}
