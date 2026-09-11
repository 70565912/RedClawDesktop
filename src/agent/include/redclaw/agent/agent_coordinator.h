#pragma once

#include <memory>
#include <vector>

#include "redclaw/agent/coordination.h"

namespace redclaw::agent {

enum class AgentCoordinationResultKind { kRemoteDurable, kDispatch, kLocalSnapshot, kRejected };
struct AgentCoordinationResult {
    AgentCoordinationResultKind kind = AgentCoordinationResultKind::kRejected;
    redclaw::protocol::AgentMessageEnvelopeV1 message;
    std::uint64_t cookie = 0;
    std::uint64_t enqueued_at_ms = 0;
    std::uint64_t persisted_at_ms = 0;
    std::string error;
};
struct AgentCoordinatorSnapshot {
    bool ready = false;
    bool sync_required = false;
    CoordinationAuthorityV1 authority = CoordinationAuthorityV1::kNone;
    std::uint64_t parsed_records = 0;
    std::uint64_t rejected_total = 0;
    std::size_t queue_depth = 0;
    std::string error;
};

class AgentCoordinator final {
public:
    explicit AgentCoordinator(NormalAgentControlConfigV1 config);
    ~AgentCoordinator();
    [[nodiscard]] bool enqueue(redclaw::protocol::AgentMessageEnvelopeV1 message,
        bool remote, std::uint64_t cookie = 0, std::string* error = nullptr);
    [[nodiscard]] std::vector<AgentCoordinationResult> take_results(std::size_t limit = 8);
    [[nodiscard]] AgentCoordinatorSnapshot snapshot() const;
    void request_sync(std::string task_id = {});
private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace redclaw::agent
