#pragma once

#include <algorithm>
#include <deque>

#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw/input/input_module.h"

namespace redclaw::runtime {

inline void discard_obsolete_remote_input(
    std::deque<redclaw::protocol::StreamControlMessageV1>& messages,
    bool retain_empty_sync = false) {
    // Boundaries discard ordinary input, never Stop. A planned capture restart
    // may retain empty syncs: they only release held state and renew the lease.
    std::erase_if(messages, [retain_empty_sync](const auto& message) {
        using Type = redclaw::protocol::StreamControlMessageTypeV1;
        return message.type != Type::kInputReleaseAll
            && !(message.type == Type::kInputControlRequest && !message.input_requested_active)
            && !(retain_empty_sync && message.type == Type::kInputStateSync
                && message.pressed_scan_codes.empty() && message.pressed_mouse_buttons == 0);
    });
}

inline bool pause_remote_input_for_capture(
    redclaw::input::RemoteInputSession& session,
    std::deque<redclaw::protocol::StreamControlMessageV1>& messages) {
    using Type = redclaw::protocol::StreamControlMessageTypeV1;
    const bool requested = std::any_of(messages.begin(), messages.end(), [](const auto& message) {
        return message.type == Type::kInputControlRequest && message.input_requested_active;
    });
    // A backend transition is not a new input failure when control is already
    // paused. Keep its original cause, while still rejecting a fresh Start.
    const bool pause = session.state() == redclaw::input::RemoteInputSessionState::kActive
        || (requested && session.authorized());
    if (pause) session.pause(redclaw::input::RemoteInputPauseReason::kNoVideo);
    discard_obsolete_remote_input(messages);
    return pause;
}

}  // namespace redclaw::runtime
