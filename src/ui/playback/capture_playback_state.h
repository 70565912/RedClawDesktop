#pragma once
#include "redclaw/protocol/stream_control_protocol.h"
#include <algorithm>

namespace redclaw::ui {
class CapturePlaybackState {
public:
    void observe(const redclaw::protocol::StreamControlMessageV1& message) {
        if (message.capture_status_version != 1 || message.capture_status < 1 || message.capture_status > 3
            || message.capture_generation == 0 || message.capture_generation < generation_) { return; }
        supported_ = true;
        generation_ = message.capture_generation;
        status_ = message.capture_status;
        first_frame_id_ = message.capture_first_frame_id;
        if (waiting()) { rearm_required_ = true; }
    }
    void presented(std::uint64_t frame_id) { presented_frame_id_ = std::max(presented_frame_id_, frame_id); }
    bool waiting() const {
        return supported_ && (status_ != 1 || first_frame_id_ == 0 || presented_frame_id_ < first_frame_id_);
    }
    bool retry_available() const { return supported_ && waiting(); }
    bool request_control() {
        if (waiting()) { return false; }
        rearm_required_ = false;
        return true;
    }
    bool control_allowed() const { return !waiting() && !rearm_required_; }
private:
    bool supported_ = false;
    bool rearm_required_ = false;
    std::uint32_t status_ = 0;
    std::uint64_t generation_ = 0, first_frame_id_ = 0, presented_frame_id_ = 0;
};
}
