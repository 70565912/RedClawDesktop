#pragma once
#include "redclaw/protocol/stream_control_protocol.h"
#include <algorithm>

namespace redclaw::ui {
class CapturePlaybackState {
public:
    void begin_navigation() {
        if (navigation_pending_) return;
        navigation_pending_ = true;
        navigation_generation_ = generation_;
    }
    void cancel_navigation() { navigation_pending_ = false; }
    void observe(const redclaw::protocol::StreamControlMessageV1& message) {
        if (message.capture_status_version != 1 || message.capture_status < 1 || message.capture_status > 3
            || message.capture_generation == 0 || message.capture_generation < generation_) { return; }
        supported_ = true;
        generation_ = message.capture_generation;
        status_ = message.capture_status;
        first_frame_id_ = message.capture_first_frame_id;
        if (status_ != 1) navigation_pending_ = false;
        if (waiting() && !navigation_pending_) { rearm_required_ = true; }
        complete_navigation();
    }
    void presented(std::uint64_t frame_id) {
        presented_frame_id_ = std::max(presented_frame_id_, frame_id);
        complete_navigation();
    }
    bool waiting() const {
        return supported_ && (status_ != 1 || first_frame_id_ == 0 || presented_frame_id_ < first_frame_id_);
    }
    bool retry_available() const { return supported_ && waiting() && !navigation_pending_; }
    bool request_control() {
        if (waiting()) { return false; }
        rearm_required_ = false;
        return true;
    }
    bool control_allowed() const { return !waiting() && !rearm_required_; }
    bool rearm_required() const { return rearm_required_; }
private:
    void complete_navigation() {
        if (navigation_pending_ && generation_ > navigation_generation_ && !waiting())
            navigation_pending_ = false;
    }
    bool supported_ = false;
    bool rearm_required_ = false;
    bool navigation_pending_ = false;
    std::uint64_t navigation_generation_ = 0;
    std::uint32_t status_ = 0;
    std::uint64_t generation_ = 0, first_frame_id_ = 0, presented_frame_id_ = 0;
};
}
