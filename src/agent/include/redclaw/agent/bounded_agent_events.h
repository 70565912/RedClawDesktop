#pragma once

#include <algorithm>
#include <deque>
#include <optional>

#include "redclaw/agent/remote_agent_broker.h"

namespace redclaw::agent {

// Requires the owner's queue mutex, never a Broker/Provider cross-call lock.
// Retain critical records; exhausted critical capacity produces an explicit
// terminal failure in one reserved slot instead of unbounded allocation.
class BoundedAgentEvents final {
public:
    void push_back(AgentProviderEvent event) {
        if (failed_) return;
        while ((events_.size() >= 512 || bytes_ + size(event) > 4U * 1024U * 1024U)
            && !events_.empty()) {
            auto found = std::find_if(events_.begin(), events_.end(), [](const auto& item) {
                return !item.approval_request && !item.terminal;
            });
            if (found == events_.end()) break;
            bytes_ -= size(*found);
            events_.erase(found);
            event.error_code = "provider_output_gap";
        }
        if (events_.size() >= 512 || bytes_ + size(event) > 4U * 1024U * 1024U) {
            failed_ = true;
            failure_ = AgentProviderEvent{
                .task_id = event.task_id,
                .event_kind = "agent_queue_exhausted",
                .text = "Agent critical event queue exhausted; synchronization required",
                .error_code = "agent_queue_exhausted",
                .state = redclaw::protocol::AgentTaskStateV1::kFailed,
                .terminal = true,
            };
            return;
        }
        bytes_ += size(event);
        events_.push_back(std::move(event));
    }
    [[nodiscard]] bool empty() const { return events_.empty() && !failure_; }
    [[nodiscard]] bool failed() const { return failed_; }
    AgentProviderEvent take_front() {
        if (events_.empty()) {
            auto result = std::move(*failure_);
            failure_.reset();
            return result;
        }
        bytes_ -= size(events_.front());
        auto result = std::move(events_.front());
        events_.pop_front();
        return result;
    }
private:
    static std::size_t size(const AgentProviderEvent& event) {
        return event.text.size() + event.task_id.size() + event.request_id.size()
            + event.event_kind.size() + event.error_code.size() + 128;
    }
    std::deque<AgentProviderEvent> events_;
    std::optional<AgentProviderEvent> failure_;
    std::size_t bytes_ = 0;
    bool failed_ = false;
};

}  // namespace redclaw::agent
