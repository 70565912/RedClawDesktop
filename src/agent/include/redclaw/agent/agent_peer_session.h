#pragma once

#include <functional>
#include <set>

#include "redclaw/agent/agent_executor.h"

namespace redclaw::agent {

enum class AgentMessageRoute { kLocalExecutor, kRemoteTaskClient, kInvalid };
[[nodiscard]] AgentMessageRoute agent_message_route(redclaw::protocol::AgentMessageTypeV1 type);

struct AgentPeerSnapshot {
    bool channel_open = false;
    bool remote_authorized = false;
    std::uint64_t generation = 0;
    std::uint64_t sent_requests = 0;
    std::uint64_t sent_results = 0;
    std::uint64_t received_commands = 0;
    std::uint64_t received_results = 0;
    std::uint64_t received_task_events = 0;
    std::uint64_t rejected_total = 0;
    std::uint64_t stale_total = 0;
    std::uint64_t last_wire_message_id = 0;
    std::uint64_t max_pump_us = 0;
    std::size_t request_depth = 0;
    std::size_t result_depth = 0;
    std::size_t request_peak = 0;
    std::size_t result_peak = 0;
    std::size_t client_sync_pending = 0;
};

// Desktop role deliberately does not appear in this contract. The runtime owns
// this session and drains network callbacks before destruction. Provider work
// stays on AgentExecutor; network/client callbacks must be bounded/nonblocking.
class AgentPeerSession final {
public:
    using Message = redclaw::protocol::AgentMessageEnvelopeV1;
    using Deliver = std::function<bool(const Message&, std::string*)>;
    explicit AgentPeerSession(std::unique_ptr<AgentExecutor> executor, Deliver client,
        std::function<bool()> mutations_allowed = {});
    ~AgentPeerSession();

    void open(std::string local_epoch);
    void close();
    void reset();
    void request_stop();
    [[nodiscard]] bool receive(const Message& message, std::uint64_t generation,
                               std::string* error = nullptr);
    [[nodiscard]] bool enqueue_request(Message message, std::string* error = nullptr);
    void pump(const Deliver& send);
    [[nodiscard]] std::size_t request_capacity() const;
    [[nodiscard]] AgentPeerSnapshot snapshot() const;
    [[nodiscard]] AgentExecutorSnapshot executor_snapshot() const;

private:
    void queue_client_sync_locked(const std::string& task_id);
    void disconnect_locked();
    void retry_client_sync();
    std::unique_ptr<AgentExecutor> executor_;
    Deliver client_;
    std::function<bool()> mutations_allowed_;
    mutable std::mutex mutex_;
    std::deque<Message> requests_;
    std::deque<Message> results_;
    std::set<std::string> sync_tasks_;
    redclaw::protocol::AgentEpochGuardV1 incoming_;
    std::string local_epoch_;
    AgentPeerSnapshot state_;
    bool prefer_request_ = true;
    bool stopped_ = false;
};

}  // namespace redclaw::agent
