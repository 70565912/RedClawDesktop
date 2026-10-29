#pragma once

#include "runtime/input_control_boundary.h"
#include <cstdint>
#include <utility>

namespace redclaw::runtime {

// The runtime callback mutex owns access; this inbox adds no worker or lock.
class RemoteInputInbox final {
public:
    using Message = redclaw::protocol::StreamControlMessageV1;
    static constexpr std::size_t kCapacity = 64;
    struct Stats {
        std::size_t depth = 0, peak = 0;
        std::uint64_t coalesced = 0, overflows = 0, max_pending_ms = 0;
    };
    struct Batch { std::deque<Message> messages; bool overflow = false; };

    bool enqueue(Message message, std::uint64_t now_ms) {
        if (!messages_.empty() && supersedes(message, messages_.back())) {
            messages_.back() = std::move(message); ++stats_.coalesced; return true;
        }
        if (messages_.size() >= kCapacity) {
            overflow_ = true; ++stats_.overflows; return false;
        }
        if (messages_.empty()) oldest_pending_ms_ = now_ms;
        messages_.push_back(std::move(message));
        stats_.peak = (std::max)(stats_.peak, messages_.size());
        return true;
    }
    Batch take(std::uint64_t now_ms) {
        if (!messages_.empty() && now_ms >= oldest_pending_ms_)
            stats_.max_pending_ms = (std::max)(stats_.max_pending_ms, now_ms - oldest_pending_ms_);
        Batch batch; batch.messages.swap(messages_);
        batch.overflow = std::exchange(overflow_, false);
        oldest_pending_ms_ = 0;
        return batch;
    }
    void discard_obsolete() { discard_obsolete_remote_input(messages_); }
    void clear() { messages_.clear(); overflow_ = false; oldest_pending_ms_ = 0; }
    bool empty() const { return messages_.empty(); }
    bool overflow_pending() const { return overflow_; }
    bool has_stop() const {
        return std::any_of(messages_.begin(), messages_.end(), [](const auto& message) {
            using Type = redclaw::protocol::StreamControlMessageTypeV1;
            return message.type == Type::kInputReleaseAll
                || (message.type == Type::kInputControlRequest && !message.input_requested_active);
        });
    }
    Stats stats() const { auto result = stats_; result.depth = messages_.size(); return result; }
private:
    static bool supersedes(const Message& next, const Message& previous) {
        using Type = redclaw::protocol::StreamControlMessageTypeV1;
        if (next.type != previous.type || next.session_epoch != previous.session_epoch
            || next.desktop_geometry_revision != previous.desktop_geometry_revision
            || next.input_sequence <= previous.input_sequence) return false;
        if (next.type == Type::kInputStateSync)
            return next.pressed_scan_codes.empty() && previous.pressed_scan_codes.empty()
                && next.pressed_mouse_buttons == 0 && previous.pressed_mouse_buttons == 0;
        const auto motion_only = [](const Message& message) {
            return !message.input_events.empty()
                && std::all_of(message.input_events.begin(), message.input_events.end(), [](const auto& event) {
                    return event.type == redclaw::protocol::RemoteInputEventTypeV1::kMouseMove;
                });
        };
        return next.type == Type::kInputBatch && motion_only(next) && motion_only(previous);
    }
    std::deque<Message> messages_;
    Stats stats_;
    bool overflow_ = false;
    std::uint64_t oldest_pending_ms_ = 0;
};

} // namespace redclaw::runtime
