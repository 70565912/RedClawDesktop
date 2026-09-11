#pragma once

#include <condition_variable>
#include <thread>

#include "redclaw/agent/remote_agent_broker.h"

namespace redclaw::agent {

struct AgentExecutorSnapshot {
    AgentExecutionSnapshot execution;
    RemoteAgentBrokerMetrics broker;
    std::size_t queued_commands = 0;
    std::size_t queued_bytes = 0;
    std::size_t outbound_count = 0;
    std::size_t cached_event_bytes = 0;
    std::uint64_t rejected_total = 0;
    std::uint64_t processed_total = 0;
    std::uint64_t max_queue_wait_ms = 0;
    std::uint64_t max_processing_ms = 0;
    std::string last_error;
};

// Broker and providers belong to one worker. No desktop/network caller takes
// a Broker lock or waits for a provider, workspace command, or journal write.
class AgentExecutor final {
public:
    explicit AgentExecutor(std::unique_ptr<RemoteAgentBroker> broker);
    ~AgentExecutor();
    AgentExecutor(const AgentExecutor&) = delete;
    AgentExecutor& operator=(const AgentExecutor&) = delete;

    void connect(std::string epoch);
    void disconnect();
    [[nodiscard]] bool handle_message(
        const redclaw::protocol::AgentMessageEnvelopeV1& message,
        std::string* error = nullptr);
    [[nodiscard]] std::vector<redclaw::protocol::AgentMessageEnvelopeV1>
        take_outbound(std::size_t limit = 8);
    [[nodiscard]] AgentExecutorSnapshot snapshot() const;
    void request_stop();

private:
    struct Command {
        redclaw::protocol::AgentMessageEnvelopeV1 message;
        std::size_t bytes = 0;
        std::chrono::steady_clock::time_point enqueued;
    };
    void run(std::stop_token stop);
    void reject_locked(const redclaw::protocol::AgentMessageEnvelopeV1& request,
                       const std::string& error);
    std::unique_ptr<RemoteAgentBroker> broker_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Command> commands_;
    std::deque<redclaw::protocol::AgentMessageEnvelopeV1> outbound_;
    std::deque<redclaw::protocol::AgentMessageEnvelopeV1> rejections_;
    std::string desired_epoch_;
    std::uint64_t connection_revision_ = 0;
    bool stopping_ = false;
    AgentExecutorSnapshot snapshot_;
    std::jthread worker_;
};

}  // namespace redclaw::agent
