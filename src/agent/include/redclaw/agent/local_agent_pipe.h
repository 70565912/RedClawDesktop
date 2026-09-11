#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "redclaw/protocol/agent_protocol.h"

namespace redclaw::agent {

struct LocalAgentPipeStats {
    bool connected = false;
    std::size_t received_depth = 0;
    std::size_t send_depth = 0;
    std::uint64_t received_total = 0;
    std::uint64_t sent_total = 0;
    std::uint64_t rejected_total = 0;
    std::uint64_t connection_total = 0;
    std::string error;
};

// Owner-user-only local transport. The I/O thread never calls GUI, Broker or
// storage code. Queue exhaustion fails only this Agent transport closed.
class LocalAgentPipe final {
public:
    LocalAgentPipe();
    ~LocalAgentPipe();
    bool start(std::string name, bool server, std::uint32_t expected_peer_pid,
               std::string* error = nullptr);
    void set_expected_peer_pid(std::uint32_t pid);
    void stop();
    [[nodiscard]] bool send(const redclaw::protocol::AgentMessageEnvelopeV1& message,
                            std::string* error = nullptr);
    [[nodiscard]] std::vector<redclaw::protocol::AgentMessageEnvelopeV1>
        take_received(std::size_t limit = 8);
    [[nodiscard]] LocalAgentPipeStats stats() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::agent
