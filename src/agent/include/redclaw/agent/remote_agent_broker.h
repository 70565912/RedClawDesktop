#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "redclaw/protocol/agent_protocol.h"

namespace redclaw::agent {
class BoundedAgentEvents;

struct AgentProjectRegistration {
    std::string project_id;
    std::string display_name;
    std::filesystem::path root;
    bool git_repository = false;
};

struct AgentProviderProbe {
    redclaw::protocol::AgentProviderKindV1 provider =
        redclaw::protocol::AgentProviderKindV1::kNone;
    bool available = false;
    redclaw::protocol::AgentProviderReadinessV1 readiness =
        redclaw::protocol::AgentProviderReadinessV1::kProbeFailed;
    std::string version;
    std::vector<std::string> models;
    bool supports_structured_approval = false;
    bool requires_turn_approval = false;
    std::string unavailable_reason;
};

struct AgentProviderTaskRequest {
    std::string task_id;
    std::string model;
    std::filesystem::path working_directory;
    std::string instruction;
};

struct AgentProviderEvent {
    std::string task_id;
    std::string request_id;
    std::string event_kind;
    std::string text;
    std::string error_code;
    redclaw::protocol::AgentTaskStateV1 state =
        redclaw::protocol::AgentTaskStateV1::kRunning;
    bool approval_request = false;
    bool terminal = false;
    bool text_delta = false;
    std::uint64_t received_at_ms = 0;
};

using AgentProviderEventSink = std::function<void(AgentProviderEvent)>;

class IAgentProvider {
public:
    virtual ~IAgentProvider() = default;

    [[nodiscard]] virtual AgentProviderProbe probe(bool force_refresh = false) = 0;
    virtual void set_event_sink(AgentProviderEventSink sink) = 0;
    virtual bool start_task(
        const AgentProviderTaskRequest& request,
        std::string* error = nullptr) = 0;
    virtual bool resume_task(
        const AgentProviderTaskRequest& request,
        std::string* error = nullptr) = 0;
    virtual bool start_turn(
        const std::string& task_id,
        const std::string& instruction,
        std::string* error = nullptr) = 0;
    virtual bool steer(
        const std::string& task_id,
        const std::string& instruction,
        std::string* error = nullptr) = 0;
    virtual bool interrupt(const std::string& task_id, std::string* error = nullptr) = 0;
    virtual bool respond_to_approval(
        const std::string& task_id,
        const std::string& request_id,
        redclaw::protocol::AgentApprovalDecisionV1 decision,
        std::string* error = nullptr) = 0;
    virtual void shutdown() = 0;
};

class IAgentWorkspaceManager {
public:
    virtual ~IAgentWorkspaceManager() = default;
    virtual bool prepare(
        const AgentProjectRegistration& project,
        const std::string& task_id,
        redclaw::protocol::AgentWorkDirectoryModeV1 mode,
        std::filesystem::path* working_directory,
        std::string* error = nullptr) = 0;
};

class GitAgentWorkspaceManager final : public IAgentWorkspaceManager {
public:
    bool prepare(
        const AgentProjectRegistration& project,
        const std::string& task_id,
        redclaw::protocol::AgentWorkDirectoryModeV1 mode,
        std::filesystem::path* working_directory,
        std::string* error = nullptr) override;
};

struct RemoteAgentBrokerConfig {
    bool authorized = false;
    std::size_t max_queued_turns = 8;
    std::size_t max_tasks = 20;
    std::size_t max_event_bytes_per_task = 4U * 1024U * 1024U;
    std::size_t max_total_event_bytes = 16U * 1024U * 1024U;
    std::size_t max_outbound_messages = 512;
    std::uint64_t approval_timeout_ms = 60U * 1000U;
    std::filesystem::path metadata_path;
    // Immutable, nonblocking callback; may be read by network and worker threads.
    std::function<bool()> mutations_allowed;
};

[[nodiscard]] bool agent_request_mutates_workspace(redclaw::protocol::AgentMessageTypeV1 type);

struct RemoteAgentBrokerMetrics {
    std::uint64_t capability_refresh_total = 0;
    std::uint64_t task_create_total = 0;
    std::uint64_t task_complete_total = 0;
    std::uint64_t task_failed_total = 0;
    std::uint64_t duplicate_task_rejected_total = 0;
    std::uint64_t event_total = 0;
    std::uint64_t event_ack_total = 0;
    std::uint64_t replayed_event_total = 0;
    std::uint64_t gap_total = 0;
    std::uint64_t approval_request_total = 0;
    std::uint64_t approval_accept_total = 0;
    std::uint64_t approval_reject_total = 0;
    std::uint64_t approval_timeout_total = 0;
    std::size_t queue_peak = 0;
    std::size_t cached_event_bytes_peak = 0;
    std::size_t outbound_queue_peak = 0;
};

struct AgentExecutionSnapshot {
    bool authorized = false;
    std::string task_id;
    redclaw::protocol::AgentTaskStateV1 state = redclaw::protocol::AgentTaskStateV1::kUnavailable;
    std::size_t queued_turns = 0;
};

class RemoteAgentBroker final {
public:
    explicit RemoteAgentBroker(
        RemoteAgentBrokerConfig config,
        std::unique_ptr<IAgentWorkspaceManager> workspace_manager =
            std::make_unique<GitAgentWorkspaceManager>());
    ~RemoteAgentBroker();

    RemoteAgentBroker(const RemoteAgentBroker&) = delete;
    RemoteAgentBroker& operator=(const RemoteAgentBroker&) = delete;

    void add_project(AgentProjectRegistration project);
    void add_provider(std::unique_ptr<IAgentProvider> provider);
    void set_authorized(bool authorized);
    [[nodiscard]] bool allows_workspace_mutations() const;
    void connect(std::string session_epoch);
    void disconnect();
    void tick();
    void shutdown();
    void refresh_provider_capabilities();

    [[nodiscard]] bool handle_message(
        const redclaw::protocol::AgentMessageEnvelopeV1& message,
        std::string* error = nullptr);
    [[nodiscard]] std::vector<redclaw::protocol::AgentMessageEnvelopeV1> take_outbound(
        std::size_t max_messages = 64);
    [[nodiscard]] std::size_t queued_turn_count() const;
    [[nodiscard]] std::size_t cached_event_bytes() const;
    [[nodiscard]] std::size_t queued_outbound_count() const;
    [[nodiscard]] RemoteAgentBrokerMetrics metrics() const;
    [[nodiscard]] AgentExecutionSnapshot execution_snapshot() const;

private:
    struct TaskRecord;
    void publish_capabilities_locked();
    void publish_projects_locked();
    void publish_snapshot_locked(TaskRecord& task, std::uint64_t after_sequence);
    void pump_snapshot_locked(TaskRecord& task, std::size_t limit);
    void append_provider_event(AgentProviderEvent event);
    void drain_provider_events();
    void append_event_locked(TaskRecord& task, AgentProviderEvent event);
    void enqueue_outbound_locked(redclaw::protocol::AgentMessageEnvelopeV1 message);
    bool start_task_locked(
        TaskRecord& task,
        const redclaw::protocol::AgentMessageEnvelopeV1& message,
        std::string* error);
    void start_next_queued_locked();
    void trim_event_cache_locked(TaskRecord* reserve_task = nullptr, std::size_t reserve_bytes = 0);
    void load_completed_metadata_locked();
    void persist_completed_metadata_locked();
    IAgentProvider* provider_locked(redclaw::protocol::AgentProviderKindV1 provider);

    RemoteAgentBrokerConfig config_;
    std::unique_ptr<IAgentWorkspaceManager> workspace_manager_;
    mutable std::mutex mutex_;
    std::mutex provider_events_mutex_;
    std::unique_ptr<BoundedAgentEvents> provider_events_;
    std::vector<AgentProjectRegistration> projects_;
    std::vector<std::unique_ptr<IAgentProvider>> providers_;
    std::deque<std::string> task_order_;
    std::deque<redclaw::protocol::AgentMessageEnvelopeV1> queued_turns_;
    std::unordered_map<std::string, std::unique_ptr<TaskRecord>> tasks_;
    std::deque<redclaw::protocol::AgentMessageEnvelopeV1> outbound_;
    std::optional<std::string> active_task_id_;
    std::string session_epoch_;
    std::uint64_t next_message_id_ = 1;
    redclaw::protocol::AgentEpochGuardV1 incoming_guard_;
    std::size_t total_event_bytes_ = 0;
    RemoteAgentBrokerMetrics metrics_;
    bool connected_ = false;
    bool shutting_down_ = false;
    bool start_next_pending_ = false;
    bool capacity_failed_ = false;
    bool metadata_write_blocked_ = false;
};

[[nodiscard]] bool load_agent_project_manifest(
    const std::filesystem::path& manifest_path,
    std::vector<AgentProjectRegistration>* projects,
    std::string* error = nullptr);

}  // namespace redclaw::agent
