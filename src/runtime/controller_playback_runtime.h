#pragma once
#include "redclaw/helper/playback_state.h"
#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw/session/session_module.h"
#include <functional>
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
        const auto state = source_ ? source_() : helper::PlaybackStateSnapshot{};
        if (state.valid) recovery_.set_playback_state(state.session, state.revision, state.paused);
        return state;
    }

    void stamp_feedback(protocol::StreamControlMessageV1& message) {
        const auto state = synchronize();
        message.playback_feedback_version = 1;
        message.playback_session = state.session;
        message.playback_revision = state.revision;
        message.playback_paused = state.paused;
        message.decode_sample_valid = state.valid && !state.paused && !state.resynchronizing
            && recovery_.snapshot().state == session::ControllerDecoderRecoveryState::kSynchronized;
    }

private:
    session::ControllerDecoderRecoveryCoordinator& recovery_;
    SnapshotSource source_;
};
}  // namespace redclaw::runtime
