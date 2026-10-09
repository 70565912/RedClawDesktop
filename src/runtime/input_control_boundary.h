#pragma once

#include <algorithm>
#include <deque>

#include "redclaw/protocol/stream_control_protocol.h"

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

}  // namespace redclaw::runtime
