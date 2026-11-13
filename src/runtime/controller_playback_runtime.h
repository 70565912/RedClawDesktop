#pragma once
#include "redclaw/helper/playback_state.h"
#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw/session/session_module.h"
#include <functional>
#include <mutex>
#include <utility>

namespace redclaw::runtime {
// Owns the local playback/recovery integration. A future negotiated sender
// hint can consume this snapshot; no sender pacing or input policy is changed.
class ControllerPlaybackRuntime final {
public:
    using SnapshotSource = std::function<helper::PlaybackStateSnapshot()>;
    ControllerPlaybackRuntime(session::ControllerDecoderRecoveryCoordinator& recovery, SnapshotSource source)
        : recovery_(recovery), source_(std::move(source)) {}

    helper::PlaybackStateSnapshot synchronize() {
        // Apply shared reads in order even when media and local control race.
        std::lock_guard lock(mutex_);
        const auto state = source_ ? source_() : helper::PlaybackStateSnapshot{};
        if (state.valid) recovery_.set_playback_state(state.session, state.revision, state.paused);
        return state;
    }

    static session::ControllerPlaybackIdentity identity(const helper::PlaybackStateSnapshot& state) {
        return {state.session, state.revision, state.paused};
    }

    static bool matches_feedback(const protocol::StreamControlMessageV1& message,
                                 const helper::PlaybackStateSnapshot& state) {
        return state.valid && message.playback_feedback_version == 1
            && message.playback_session == state.session && message.playback_revision == state.revision
            && message.playback_paused == state.paused
            && (message.type != protocol::StreamControlMessageTypeV1::kKeyframeRequest || !state.paused);
    }

    // Manual capture retry must reach the Host even when playback is paused,
    // unmapped, or stamped with a different session or revision.
    static bool should_consume_stale_playback_command(
        const protocol::StreamControlMessageV1& message,
        const helper::PlaybackStateSnapshot& state) {
        if (message.type == protocol::StreamControlMessageTypeV1::kReceiverNetworkStats) {
            return !matches_feedback(message, state);
        }
        if (message.type == protocol::StreamControlMessageTypeV1::kKeyframeRequest
            && !message.capture_retry_requested) {
            return !matches_feedback(message, state);
        }
        return false;
    }

    static void stamp_playback_identity(protocol::StreamControlMessageV1& message,
                                        const helper::PlaybackStateSnapshot& state) {
        message.playback_feedback_version = 1;
        message.playback_session = state.session;
        message.playback_revision = state.revision;
        message.playback_paused = state.paused;
    }

    void stamp_feedback(protocol::StreamControlMessageV1& message,
                        const helper::PlaybackStateSnapshot& state) {
        const auto current = synchronize();
        stamp_playback_identity(message, state);
        message.decode_sample_valid = helper::playback_output_is_current(state, current)
            && !state.resynchronizing && !current.resynchronizing
            && recovery_.snapshot().state == session::ControllerDecoderRecoveryState::kSynchronized;
    }

private:
    session::ControllerDecoderRecoveryCoordinator& recovery_;
    SnapshotSource source_;
    std::mutex mutex_;
};
}  // namespace redclaw::runtime
