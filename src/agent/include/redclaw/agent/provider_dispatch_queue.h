#pragma once

#include <condition_variable>
#include <mutex>
#include <variant>
#include "redclaw/agent/bounded_agent_events.h"

namespace redclaw::agent {
struct ProviderProcessEvent {
    enum class Kind { kStdout, kStderr, kExit, kOverflow };
    Kind kind;
    std::string line;
    int exit_code = 0;
    std::uint64_t generation = 0;
};

// Pipe callbacks never acquire Provider/Broker business locks. Both raw input
// and parsed output use the Provider's existing serial dispatch thread.
class ProviderDispatchQueue final {
public:
    using Item = std::variant<ProviderProcessEvent, AgentProviderEvent>;
    void push_process(ProviderProcessEvent event) {
        std::unique_lock lock(mutex_);
        // Backpressure only the dedicated process pipe readers. Brief stdout
        // bursts must not kill a task or discard JSON containing an approval.
        // stop() wakes these readers before process teardown joins them.
        changed_.wait(lock, [&] {
            return stopping_ || source_failed_ || event.line.size() > 4U * 1024U * 1024U
                || (sources_.size() < 512
                    && source_bytes_ + event.line.size() <= 4U * 1024U * 1024U);
        });
        if (stopping_ || source_failed_) return;
        if (sources_.size() >= 512 || source_bytes_ + event.line.size() > 4U * 1024U * 1024U) {
            source_failed_ = true;
            // Raw JSON can contain approvals, so do not silently drop it as text.
            sources_.push_back({ProviderProcessEvent::Kind::kOverflow, {}, 125, event.generation});
        } else {
            source_bytes_ += event.line.size();
            sources_.push_back(std::move(event));
        }
        changed_.notify_one();
    }
    void push_back(AgentProviderEvent event) {
        std::lock_guard lock(mutex_);
        if (!stopping_) events_.push_back(std::move(event));
        changed_.notify_one();
    }
    bool failed() const {
        std::lock_guard lock(mutex_);
        return source_failed_ || events_.failed();
    }
    std::optional<Item> take() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return stopping_ || !events_.empty() || !sources_.empty(); });
        if (stopping_) return std::nullopt;
        if (!events_.empty()) return events_.take_front();
        source_bytes_ -= sources_.front().line.size();
        auto event = std::move(sources_.front());
        sources_.pop_front();
        changed_.notify_all();
        return event;
    }
    void stop() {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        sources_.clear();
        source_bytes_ = 0;
        changed_.notify_all();
    }
private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<ProviderProcessEvent> sources_;
    BoundedAgentEvents events_;
    std::size_t source_bytes_ = 0;
    bool source_failed_ = false;
    bool stopping_ = false;
};
}  // namespace redclaw::agent
