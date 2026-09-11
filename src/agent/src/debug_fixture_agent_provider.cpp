#include "redclaw/agent/agent_providers.h"
#include <chrono>
#include <iostream>

namespace redclaw::agent {
#ifndef NDEBUG
namespace {
// Explicit test provider: no CLI, instruction execution, filesystem or account
// access. Consent, worktrees, approvals and event limits still use real Broker.
class DebugFixtureProvider final : public IAgentProvider {
public:
    AgentProviderProbe probe(bool) override {
        return {.provider = redclaw::protocol::AgentProviderKindV1::kCodex,
            .available = true,
            .readiness = redclaw::protocol::AgentProviderReadinessV1::kReady,
            .version = "debug-fixture-1", .requires_turn_approval = true};
    }
    void set_event_sink(AgentProviderEventSink sink) override { sink_ = std::move(sink); }
    bool start_task(const AgentProviderTaskRequest& request, std::string*) override {
        return request_approval(request.task_id);
    }
    bool resume_task(const AgentProviderTaskRequest& request, std::string* error) override {
        return start_task(request, error);
    }
    bool start_turn(const std::string& task, const std::string&, std::string*) override {
        return request_approval(task);
    }
    bool steer(const std::string& task, const std::string&, std::string*) override {
        sink_({.task_id = task, .event_kind = "fixture_steer", .text = "fixture acknowledged"});
        return true;
    }
    bool interrupt(const std::string& task, std::string*) override {
        pending_task_.clear();
        pending_approval_.clear();
        sink_({.task_id = task, .event_kind = "interrupted", .text = "fixture interrupted",
            .state = redclaw::protocol::AgentTaskStateV1::kInterrupted, .terminal = true});
        return true;
    }
    bool respond_to_approval(const std::string& task, const std::string& approval,
        redclaw::protocol::AgentApprovalDecisionV1 decision, std::string*) override {
        if (pending_task_.empty() || task != pending_task_ || approval != pending_approval_) return false;
        pending_task_.clear();
        pending_approval_.clear();
        if (decision == redclaw::protocol::AgentApprovalDecisionV1::kAccept) return output(task);
        sink_({.task_id = task, .event_kind = "turn_rejected", .text = "Debug fixture rejected",
            .state = redclaw::protocol::AgentTaskStateV1::kPaused});
        return true;
    }
    void shutdown() override { pending_task_.clear(); pending_approval_.clear(); }
private:
    bool request_approval(const std::string& task) {
        if (!pending_task_.empty()) return false;
        pending_task_ = task;
        pending_approval_ = "debug-turn-" + std::to_string(++next_approval_);
        sink_({.task_id = task, .request_id = pending_approval_, .event_kind = "debug_turn_preapproval",
            .text = "Approve this Debug-only 1 MiB output fixture (no command execution)",
            .state = redclaw::protocol::AgentTaskStateV1::kAwaitingApproval, .approval_request = true});
        return true;
    }
    bool output(const std::string& task) {
        const auto first_output_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        for (unsigned i = 0; i < 128; ++i)
            sink_({.task_id = task, .event_kind = "text_delta",
                .text = std::string(8192, 'x'), .text_delta = true});
        sink_({.task_id = task, .event_kind = "completed", .text = "Debug fixture: 1 MiB emitted",
            .state = redclaw::protocol::AgentTaskStateV1::kCompleted, .terminal = true});
        // One local diagnostic per task, after emission; no text payload or
        // network envelope changes. Both local runtime processes share QPC.
        std::cout << "Agent fixture timing task_id=" << task << " first_output_us="
            << first_output_us << " bytes=1048576\n";
        return true;
    }
    AgentProviderEventSink sink_;
    std::string pending_task_, pending_approval_;
    std::uint64_t next_approval_ = 0;
};
}  // namespace
#endif
std::unique_ptr<IAgentProvider> make_debug_fixture_agent_provider() {
#ifndef NDEBUG
    return std::make_unique<DebugFixtureProvider>();
#else
    return nullptr;
#endif
}
}  // namespace redclaw::agent
