#include "redclaw/agent/remote_agent_broker.h"
#include "persisted_agent_metadata.h"
#include "redclaw/agent/bounded_agent_events.h"
#include "owned_agent_job.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace redclaw::agent {
namespace {

std::uint64_t unix_time_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::uint64_t steady_time_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void assign_error(std::string value, std::string* error) {
    if (error != nullptr) {
        *error = std::move(value);
    }
}

bool safe_id(std::string_view value) {
    if (value.empty() || value.size() > 128U) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.';
    });
}

bool terminal_state(redclaw::protocol::AgentTaskStateV1 state) {
    return state == redclaw::protocol::AgentTaskStateV1::kCompleted
        || state == redclaw::protocol::AgentTaskStateV1::kFailed
        || state == redclaw::protocol::AgentTaskStateV1::kInterrupted;
}

std::size_t event_size(const AgentProviderEvent& event) {
    return event.task_id.size() + event.request_id.size() + event.event_kind.size()
        + event.text.size() + event.error_code.size() + 96U;
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}

void replace_all_case_insensitive(
    std::string* text,
    const std::string& needle,
    std::string_view replacement) {
    if (text == nullptr || needle.empty()) {
        return;
    }
    std::string lowered_text = *text;
    std::string lowered_needle = needle;
    std::transform(lowered_text.begin(), lowered_text.end(), lowered_text.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::transform(lowered_needle.begin(), lowered_needle.end(), lowered_needle.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::size_t offset = 0;
    while ((offset = lowered_text.find(lowered_needle, offset)) != std::string::npos) {
        text->replace(offset, needle.size(), replacement);
        lowered_text.replace(offset, needle.size(), replacement);
        offset += replacement.size();
    }
}

void redact_windows_absolute_paths(std::string* text) {
    if (text == nullptr) {
        return;
    }
    std::size_t index = 0;
    while (index + 2U < text->size()) {
        const bool boundary = index == 0
            || std::isalnum(static_cast<unsigned char>((*text)[index - 1U])) == 0;
        const bool drive_path = boundary
            && std::isalpha(static_cast<unsigned char>((*text)[index])) != 0
            && (*text)[index + 1U] == ':'
            && ((*text)[index + 2U] == '/' || (*text)[index + 2U] == '\\');
        if (!drive_path) {
            ++index;
            continue;
        }
        std::size_t end = index + 3U;
        while (end < text->size()) {
            const char ch = (*text)[end];
            if (ch == '\r' || ch == '\n' || ch == '"' || ch == '\'' || ch == '`'
                || ch == '<' || ch == '>' || ch == ')' || ch == ']' || ch == '}'
                || ch == ',' || ch == ';') {
                break;
            }
            ++end;
        }
        text->replace(index, end - index, "[HOST_PATH]");
        index += std::string_view("[HOST_PATH]").size();
    }
}

#ifdef _WIN32
std::wstring quote_argument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'\"') {
            quoted.append(slashes * 2U + 1U, L'\\');
            quoted.push_back(ch);
            slashes = 0;
            continue;
        }
        quoted.append(slashes, L'\\');
        slashes = 0;
        quoted.push_back(ch);
    }
    quoted.append(slashes * 2U, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

bool run_git_worktree_add(
    const std::filesystem::path& root,
    const std::filesystem::path& worktree,
    const std::string& branch,
    std::string* error) {
    const std::wstring command = L"git -C " + quote_argument(root.wstring())
        + L" worktree add -b " + quote_argument(std::wstring(branch.begin(), branch.end()))
        + L" " + quote_argument(worktree.wstring()) + L" HEAD";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(
            nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, root.wstring().c_str(), &startup, &process) == FALSE) {
        assign_error("failed to start git worktree process", error);
        return false;
    }
    OwnedAgentJob job;
    const bool contained = job.attach_and_resume(process);
    CloseHandle(process.hThread);
    if (!contained) {
        CloseHandle(process.hProcess);
        assign_error("cannot isolate git worktree process", error);
        return false;
    }
    if (WaitForSingleObject(process.hProcess, 60000) != WAIT_OBJECT_0) {
        job.terminate(124);
        WaitForSingleObject(process.hProcess, 2000);
        CloseHandle(process.hProcess);
        assign_error("git worktree preparation exceeded 60 seconds", error);
        return false;
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    if (exit_code != 0) {
        assign_error("git worktree add failed with exit code " + std::to_string(exit_code), error);
        return false;
    }
    return true;
}
#endif

}  // namespace

struct RemoteAgentBroker::TaskRecord {
    struct CachedEvent {
        std::uint64_t sequence = 0;
        std::size_t bytes = 0;
        AgentProviderEvent event;
    };

    std::string task_id;
    redclaw::protocol::AgentProviderKindV1 provider =
        redclaw::protocol::AgentProviderKindV1::kNone;
    std::string model;
    std::string project_id;
    redclaw::protocol::AgentWorkDirectoryModeV1 mode =
        redclaw::protocol::AgentWorkDirectoryModeV1::kIsolatedWorktree;
    std::filesystem::path working_directory;
    redclaw::protocol::AgentTaskStateV1 state =
        redclaw::protocol::AgentTaskStateV1::kQueued;
    std::string initial_instruction;
    std::string pending_approval_request_id;
    std::uint64_t pending_approval_started_ms = 0;
    std::uint64_t next_event_sequence = 1;
    std::uint64_t acknowledged_event_sequence = 0;
    std::uint64_t evicted_through_sequence = 0;
    std::size_t event_bytes = 0;
    bool cache_gap = false;
    bool transport_gap_pending = false;
    bool cache_exhausted = false;
    std::optional<std::uint64_t> replay_after_sequence;
    std::deque<CachedEvent> events;
};

bool GitAgentWorkspaceManager::prepare(
    const AgentProjectRegistration& project,
    const std::string& task_id,
    redclaw::protocol::AgentWorkDirectoryModeV1 mode,
    std::filesystem::path* working_directory,
    std::string* error) {
    if (working_directory == nullptr || !safe_id(task_id)) {
        assign_error("invalid agent workspace request", error);
        return false;
    }
    std::error_code filesystem_error;
    if (!std::filesystem::is_directory(project.root, filesystem_error)) {
        assign_error("registered project root is unavailable", error);
        return false;
    }
    if (mode == redclaw::protocol::AgentWorkDirectoryModeV1::kDirectWorkspace) {
        *working_directory = project.root;
        return true;
    }
    if (!project.git_repository) {
        assign_error("isolated_worktree requires a registered Git repository", error);
        return false;
    }
    const std::filesystem::path parent = project.root.parent_path()
        / ".redclaw-agent-worktrees";
    std::filesystem::create_directories(parent, filesystem_error);
    if (filesystem_error) {
        assign_error("failed to create isolated worktree parent", error);
        return false;
    }
    const std::filesystem::path target = parent / task_id;
    if (std::filesystem::exists(target, filesystem_error)) {
        assign_error("isolated worktree target already exists", error);
        return false;
    }
#ifdef _WIN32
    if (!run_git_worktree_add(
            project.root, target, "redclaw/agent/" + task_id, error)) {
        return false;
    }
#else
    assign_error("isolated worktree creation is currently supported on Windows only", error);
    return false;
#endif
    *working_directory = target;
    return true;
}

RemoteAgentBroker::RemoteAgentBroker(
    RemoteAgentBrokerConfig config,
    std::unique_ptr<IAgentWorkspaceManager> workspace_manager)
    : config_(std::move(config)), workspace_manager_(std::move(workspace_manager)),
      provider_events_(std::make_unique<BoundedAgentEvents>()) {
    config_.max_tasks = std::max<std::size_t>(1U, config_.max_tasks);
    config_.max_outbound_messages = std::max<std::size_t>(1U, config_.max_outbound_messages);
    std::lock_guard lock(mutex_);
    load_completed_metadata_locked();
}

RemoteAgentBroker::~RemoteAgentBroker() {
    shutdown();
}

void RemoteAgentBroker::add_project(AgentProjectRegistration project) {
    std::lock_guard lock(mutex_);
    if (!safe_id(project.project_id) || project.display_name.empty()
        || project.display_name.size() > 256U || project.root.empty()) {
        return;
    }
    projects_.push_back(std::move(project));
}

void RemoteAgentBroker::add_provider(std::unique_ptr<IAgentProvider> provider) {
    if (provider == nullptr) {
        return;
    }
    provider->set_event_sink([this](AgentProviderEvent event) {
        event.received_at_ms = steady_time_ms();
        std::lock_guard lock(provider_events_mutex_);
        provider_events_->push_back(std::move(event));
    });
    std::lock_guard lock(mutex_);
    providers_.push_back(std::move(provider));
}

void RemoteAgentBroker::set_authorized(bool authorized) {
    std::lock_guard lock(mutex_);
    config_.authorized = authorized;
}

AgentExecutionSnapshot RemoteAgentBroker::execution_snapshot() const {
    std::lock_guard lock(mutex_);
    AgentExecutionSnapshot result;
    result.authorized = config_.authorized;
    result.queued_turns = queued_turns_.size();
    result.state = config_.authorized ? redclaw::protocol::AgentTaskStateV1::kIdle
                                     : redclaw::protocol::AgentTaskStateV1::kUnavailable;
    if (active_task_id_) {
        const auto task = tasks_.find(*active_task_id_);
        if (task != tasks_.end()) {
            result.task_id = task->first;
            result.state = task->second->state;
        }
    }
    return result;
}

void RemoteAgentBroker::connect(std::string session_epoch) {
    std::lock_guard lock(mutex_);
    session_epoch_ = std::move(session_epoch);
    connected_ = true;
    incoming_guard_.reset();
    next_message_id_ = 1;
    const bool has_queued_task = std::any_of(
        tasks_.begin(), tasks_.end(), [](const auto& pair) {
            return pair.second->state == redclaw::protocol::AgentTaskStateV1::kQueued;
        });
    if (!active_task_id_.has_value()
        && (!queued_turns_.empty() || has_queued_task)) {
        start_next_pending_ = true;
    }
    publish_capabilities_locked();
    publish_projects_locked();
}

void RemoteAgentBroker::disconnect() {
    drain_provider_events();
    std::lock_guard lock(mutex_);
    connected_ = false;
    outbound_.clear();
    incoming_guard_.reset();
    for (auto& [task_id, task] : tasks_) {
        (void)task_id;
        task->replay_after_sequence.reset();
        if (!task->pending_approval_request_id.empty()) {
            if (auto* provider = provider_locked(task->provider); provider != nullptr) {
                std::string ignored;
                (void)provider->respond_to_approval(
                    task->task_id, task->pending_approval_request_id,
                    redclaw::protocol::AgentApprovalDecisionV1::kReject, &ignored);
                (void)provider->interrupt(task->task_id, &ignored);
            }
            task->pending_approval_request_id.clear();
            task->pending_approval_started_ms = 0;
            task->state = redclaw::protocol::AgentTaskStateV1::kPaused;
            if (active_task_id_ == task->task_id) {
                active_task_id_.reset();
                start_next_pending_ = true;
            }
            ++metrics_.approval_reject_total;
            append_event_locked(*task, AgentProviderEvent{
                .task_id = task->task_id,
                .event_kind = "approval_rejected_on_disconnect",
                .text = "Pending approval was rejected because the Agent channel disconnected",
                .error_code = "agent_disconnected",
                .state = redclaw::protocol::AgentTaskStateV1::kPaused,
            });
        }
    }
}

void RemoteAgentBroker::tick() {
    drain_provider_events();
    std::lock_guard lock(mutex_);
    if (shutting_down_ || config_.approval_timeout_ms == 0) {
        return;
    }
    const std::uint64_t now_ms = steady_time_ms();
    for (auto& [task_id, task] : tasks_) {
        (void)task_id;
        if (task->pending_approval_request_id.empty()
            || task->pending_approval_started_ms == 0
            || now_ms - task->pending_approval_started_ms < config_.approval_timeout_ms) {
            continue;
        }
        if (auto* provider = provider_locked(task->provider); provider != nullptr) {
            std::string ignored;
            (void)provider->respond_to_approval(
                task->task_id, task->pending_approval_request_id,
                redclaw::protocol::AgentApprovalDecisionV1::kReject, &ignored);
            (void)provider->interrupt(task->task_id, &ignored);
        }
        task->pending_approval_request_id.clear();
        task->pending_approval_started_ms = 0;
        task->state = redclaw::protocol::AgentTaskStateV1::kPaused;
        if (active_task_id_ == task->task_id) {
            active_task_id_.reset();
            start_next_pending_ = true;
        }
        ++metrics_.approval_timeout_total;
        ++metrics_.approval_reject_total;
        append_event_locked(*task, AgentProviderEvent{
            .task_id = task->task_id,
            .event_kind = "approval_timed_out",
            .text = "Pending Agent approval timed out and was rejected",
            .error_code = "approval_timeout",
            .state = redclaw::protocol::AgentTaskStateV1::kPaused,
        });
    }
}

void RemoteAgentBroker::shutdown() {
    for (int batch = 0; batch < 9; ++batch) drain_provider_events();
    std::vector<IAgentProvider*> providers;
    {
        std::lock_guard lock(mutex_);
        if (shutting_down_) {
            return;
        }
        shutting_down_ = true;
        for (auto& provider : providers_) {
            providers.push_back(provider.get());
        }
    }
    for (auto* provider : providers) {
        provider->shutdown();
    }
}

void RemoteAgentBroker::refresh_provider_capabilities() {
    std::lock_guard lock(mutex_);
    ++metrics_.capability_refresh_total;
    for (auto& provider : providers_) {
        (void)provider->probe(true);
    }
    publish_capabilities_locked();
}

void RemoteAgentBroker::publish_capabilities_locked() {
    if (!connected_) {
        return;
    }
    if (!config_.authorized) {
        redclaw::protocol::AgentMessageEnvelopeV1 unavailable;
        unavailable.type = redclaw::protocol::AgentMessageTypeV1::kCapabilities;
        unavailable.available = false;
        unavailable.complete = true;
        unavailable.error_code = "not_authorized";
        enqueue_outbound_locked(std::move(unavailable));
        return;
    }
    for (auto& provider : providers_) {
        const auto probe = provider->probe();
        redclaw::protocol::AgentMessageEnvelopeV1 message;
        message.type = redclaw::protocol::AgentMessageTypeV1::kCapabilities;
        message.provider = probe.provider;
        message.provider_readiness = probe.readiness;
        message.available = probe.available;
        message.display_name = probe.version;
        message.requires_turn_approval = probe.requires_turn_approval;
        message.supports_structured_approval = probe.supports_structured_approval;
        message.error_code = probe.available ? std::string{} : "provider_unavailable";
        message.text = probe.available ? std::string{} : probe.unavailable_reason;
        enqueue_outbound_locked(std::move(message));
        for (const auto& model : probe.models) {
            redclaw::protocol::AgentMessageEnvelopeV1 model_message;
            model_message.type = redclaw::protocol::AgentMessageTypeV1::kCapabilities;
            model_message.provider = probe.provider;
            model_message.provider_readiness = probe.readiness;
            model_message.available = probe.available;
            model_message.model = model;
            model_message.requires_turn_approval = probe.requires_turn_approval;
            model_message.supports_structured_approval = probe.supports_structured_approval;
            enqueue_outbound_locked(std::move(model_message));
        }
    }
    redclaw::protocol::AgentMessageEnvelopeV1 complete;
    complete.type = redclaw::protocol::AgentMessageTypeV1::kCapabilities;
    complete.available = true;
    complete.complete = true;
    enqueue_outbound_locked(std::move(complete));
}

void RemoteAgentBroker::publish_projects_locked() {
    if (!connected_ || !config_.authorized) {
        return;
    }
    for (const auto& project : projects_) {
        redclaw::protocol::AgentMessageEnvelopeV1 message;
        message.type = redclaw::protocol::AgentMessageTypeV1::kProjectCatalog;
        message.project_id = project.project_id;
        message.display_name = project.display_name;
        message.git_repository = project.git_repository;
        enqueue_outbound_locked(std::move(message));
    }
    redclaw::protocol::AgentMessageEnvelopeV1 complete;
    complete.type = redclaw::protocol::AgentMessageTypeV1::kProjectCatalog;
    complete.complete = true;
    enqueue_outbound_locked(std::move(complete));
}

bool RemoteAgentBroker::handle_message(
    const redclaw::protocol::AgentMessageEnvelopeV1& message,
    std::string* error) {
    drain_provider_events();
    std::lock_guard lock(mutex_);
    if (!connected_) {
        assign_error("agent transport is disconnected", error);
        return false;
    }
    if (!incoming_guard_.accept(message, error)) {
        return false;
    }
    if (!config_.authorized) {
        assign_error("remote agent is not authorized", error);
        return false;
    }

    if (message.type == redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest) {
        if (message.task_id.empty()) {
            for (const auto& task_id : task_order_) {
                publish_snapshot_locked(*tasks_.at(task_id), message.acknowledged_event_sequence);
            }
        } else if (const auto found = tasks_.find(message.task_id); found != tasks_.end()) {
            publish_snapshot_locked(*found->second, message.acknowledged_event_sequence);
        } else {
            redclaw::protocol::AgentMessageEnvelopeV1 missing;
            missing.type = redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot;
            missing.task_id = message.task_id;
            missing.request_id = message.request_id;
            missing.event_sequence = message.acknowledged_event_sequence;
            missing.task_state = redclaw::protocol::AgentTaskStateV1::kFailed;
            missing.error_code = "task_not_found";
            missing.event_kind = "sync_unavailable";
            missing.complete = true;
            missing.text = "This Host no longer retains the task. No task was restarted or executed.";
            enqueue_outbound_locked(std::move(missing));
        }
        return true;
    }

    if (message.type == redclaw::protocol::AgentMessageTypeV1::kEventAck) {
        const auto found = tasks_.find(message.task_id);
        if (found == tasks_.end()) {
            assign_error("unknown agent task", error);
            return false;
        }
        found->second->acknowledged_event_sequence = std::max(
            found->second->acknowledged_event_sequence,
            message.acknowledged_event_sequence);
        ++metrics_.event_ack_total;
        return true;
    }

    if (message.type == redclaw::protocol::AgentMessageTypeV1::kTaskCreate) {
        trim_event_cache_locked();
        if (tasks_.size() >= config_.max_tasks) {
            const auto finished = std::find_if(task_order_.begin(), task_order_.end(), [&](const auto& id) {
                return terminal_state(tasks_.at(id)->state) && active_task_id_ != id;
            });
            if (finished != task_order_.end()) {
                total_event_bytes_ -= tasks_.at(*finished)->event_bytes;
                tasks_.erase(*finished);
                task_order_.erase(finished);
            }
        }
        if (capacity_failed_ || tasks_.size() >= config_.max_tasks) {
            assign_error("agent_busy: task or critical event capacity exhausted", error);
            return false;
        }
        {
            std::lock_guard events_lock(provider_events_mutex_);
            if (provider_events_->failed()) {
                assign_error("agent_busy: critical Provider events exhausted; Agent unavailable", error);
                return false;
            }
        }
        if (tasks_.contains(message.task_id)) {
            ++metrics_.duplicate_task_rejected_total;
            assign_error("duplicate agent task id", error);
            return false;
        }
        const std::size_t queued_tasks = static_cast<std::size_t>(std::count_if(
            tasks_.begin(), tasks_.end(), [](const auto& pair) {
                return pair.second->state == redclaw::protocol::AgentTaskStateV1::kQueued;
            }));
        if (active_task_id_.has_value()
            && queued_tasks + queued_turns_.size() >= config_.max_queued_turns) {
            assign_error("agent task queue is full", error);
            return false;
        }
        auto task = std::make_unique<TaskRecord>();
        task->task_id = message.task_id;
        task->provider = message.provider;
        task->model = message.model;
        task->project_id = message.project_id;
        task->mode = message.work_directory_mode;
        task->initial_instruction = message.text;
        TaskRecord* task_ptr = task.get();
        tasks_.emplace(task->task_id, std::move(task));
        task_order_.push_back(task_ptr->task_id);
        ++metrics_.task_create_total;
        metrics_.queue_peak = std::max(metrics_.queue_peak, queued_tasks + 1U);
        if (!active_task_id_.has_value()) {
            if (!start_task_locked(*task_ptr, message, error)) {
                task_ptr->state = redclaw::protocol::AgentTaskStateV1::kFailed;
                append_event_locked(*task_ptr, AgentProviderEvent{
                    .task_id = task_ptr->task_id,
                    .event_kind = "task_error",
                    .text = error != nullptr ? *error : "agent task failed to start",
                    .error_code = "start_failed",
                    .state = redclaw::protocol::AgentTaskStateV1::kFailed,
                    .terminal = true,
                });
                return false;
            }
        } else {
            task_ptr->state = redclaw::protocol::AgentTaskStateV1::kQueued;
            append_event_locked(*task_ptr, AgentProviderEvent{
                .task_id = task_ptr->task_id,
                .event_kind = "task_queued",
                .state = redclaw::protocol::AgentTaskStateV1::kQueued,
            });
        }
        trim_event_cache_locked();
        return true;
    }

    const auto found = tasks_.find(message.task_id);
    if (found == tasks_.end()) {
        assign_error("unknown agent task", error);
        return false;
    }
    TaskRecord& task = *found->second;
    IAgentProvider* provider = provider_locked(task.provider);
    if (provider == nullptr) {
        assign_error("agent provider is unavailable", error);
        return false;
    }
    if (message.type == redclaw::protocol::AgentMessageTypeV1::kTurnStart) {
        if (active_task_id_.has_value()) {
            const std::size_t queued_tasks = static_cast<std::size_t>(std::count_if(
                tasks_.begin(), tasks_.end(), [](const auto& pair) {
                    return pair.second->state == redclaw::protocol::AgentTaskStateV1::kQueued;
                }));
            if (queued_tasks + queued_turns_.size() >= config_.max_queued_turns) {
                assign_error("agent turn queue is full", error);
                return false;
            }
            queued_turns_.push_back(message);
            metrics_.queue_peak = std::max(
                metrics_.queue_peak, queued_turns_.size() + queued_tasks);
            append_event_locked(task, AgentProviderEvent{
                .task_id = task.task_id,
                .event_kind = "turn_queued",
                .state = redclaw::protocol::AgentTaskStateV1::kQueued,
            });
            return true;
        }
        active_task_id_ = task.task_id;
        task.state = redclaw::protocol::AgentTaskStateV1::kStarting;
        persist_completed_metadata_locked();
        if (!provider->start_turn(task.task_id, message.text, error)) {
            active_task_id_.reset();
            task.state = redclaw::protocol::AgentTaskStateV1::kFailed;
            return false;
        }
        return true;
    }
    if (message.type == redclaw::protocol::AgentMessageTypeV1::kTurnSteer) {
        if (active_task_id_ != task.task_id) {
            assign_error("agent task does not own the active turn", error);
            return false;
        }
        return provider->steer(task.task_id, message.text, error);
    }
    if (message.type == redclaw::protocol::AgentMessageTypeV1::kTurnInterrupt) {
        if (active_task_id_ != task.task_id) {
            assign_error("agent task does not own the active turn", error);
            return false;
        }
        return provider->interrupt(task.task_id, error);
    }
    if (message.type == redclaw::protocol::AgentMessageTypeV1::kApprovalDecision) {
        if (task.pending_approval_request_id != message.request_id) {
            assign_error("agent approval request is stale", error);
            return false;
        }
        const bool accepted = provider->respond_to_approval(
            task.task_id, message.request_id, message.approval_decision, error);
        if (accepted) {
            if (message.approval_decision
                == redclaw::protocol::AgentApprovalDecisionV1::kAccept) {
                ++metrics_.approval_accept_total;
            } else if (message.approval_decision
                == redclaw::protocol::AgentApprovalDecisionV1::kReject) {
                ++metrics_.approval_reject_total;
            }
            task.pending_approval_request_id.clear();
            task.pending_approval_started_ms = 0;
            if (message.approval_decision
                == redclaw::protocol::AgentApprovalDecisionV1::kReject) {
                task.state = redclaw::protocol::AgentTaskStateV1::kPaused;
                append_event_locked(task, AgentProviderEvent{
                    .task_id = task.task_id,
                    .event_kind = "approval_rejected",
                    .text = "Agent approval was rejected by the Controller",
                    .state = redclaw::protocol::AgentTaskStateV1::kPaused,
                });
                std::string ignored;
                (void)provider->interrupt(task.task_id, &ignored);
                if (active_task_id_ == task.task_id) {
                    active_task_id_.reset();
                    start_next_pending_ = true;
                }
            }
        }
        return accepted;
    }
    assign_error("agent message type is not valid from Controller", error);
    return false;
}

bool RemoteAgentBroker::start_task_locked(
    TaskRecord& task,
    const redclaw::protocol::AgentMessageEnvelopeV1& message,
    std::string* error) {
    const auto project = std::find_if(projects_.begin(), projects_.end(),
        [&](const AgentProjectRegistration& candidate) {
            return candidate.project_id == task.project_id;
        });
    if (project == projects_.end()) {
        assign_error("unknown registered project id", error);
        return false;
    }
    IAgentProvider* provider = provider_locked(task.provider);
    if (provider == nullptr) {
        assign_error("requested agent provider is unavailable", error);
        return false;
    }
    const auto capability = provider->probe();
    if (!capability.available) {
        assign_error("requested agent provider is unavailable", error);
        return false;
    }
    if (task.provider == redclaw::protocol::AgentProviderKindV1::kCodex
        && !task.model.empty()) {
        assign_error("Codex v1 uses the Host default model", error);
        return false;
    }
    if (task.provider == redclaw::protocol::AgentProviderKindV1::kCursor
        && (task.model.empty()
            || std::find(capability.models.begin(), capability.models.end(), task.model)
                == capability.models.end())) {
        assign_error("requested Cursor model is not available", error);
        return false;
    }
    if (!workspace_manager_->prepare(
            *project, task.task_id, task.mode, &task.working_directory, error)) {
        return false;
    }
    task.state = redclaw::protocol::AgentTaskStateV1::kStarting;
    active_task_id_ = task.task_id;
    persist_completed_metadata_locked();
    append_event_locked(task, AgentProviderEvent{
        .task_id = task.task_id,
        .event_kind = "task_starting",
        .state = redclaw::protocol::AgentTaskStateV1::kStarting,
    });
    AgentProviderTaskRequest request{
        .task_id = task.task_id,
        .model = task.model,
        .working_directory = task.working_directory,
        .instruction = message.text,
    };
    if (capacity_failed_) {
        assign_error("agent_busy: critical event capacity exhausted", error);
        return false;
    }
    if (!provider->start_task(request, error)) {
        active_task_id_.reset();
        return false;
    }
    return true;
}

void RemoteAgentBroker::append_provider_event(AgentProviderEvent event) {
    std::lock_guard lock(mutex_);
    if (shutting_down_ || capacity_failed_) {
        return;
    }
    const auto found = tasks_.find(event.task_id);
    if (found == tasks_.end()) {
        return;
    }
    TaskRecord& task = *found->second;
    // Broker consent/lifecycle decisions own paused and finished turns. Providers may
    // acknowledge reject + interrupt asynchronously (including after reconnect).
    // Those callbacks must neither reopen approval nor replace paused with a
    // spurious failure. An explicit new turn takes ownership and sets starting.
    if ((task.state == redclaw::protocol::AgentTaskStateV1::kPaused || terminal_state(task.state))
        && active_task_id_ != task.task_id) {
        return;
    }
    if (!event.text.empty()) {
        const auto redact_registered_path = [&](const std::filesystem::path& path,
                                                 std::string_view marker) {
            if (path.empty()) {
                return;
            }
            const std::string native = path_utf8(path);
            replace_all_case_insensitive(&event.text, native, marker);
            std::string alternate = native;
            std::replace(alternate.begin(), alternate.end(), '\\', '/');
            replace_all_case_insensitive(&event.text, alternate, marker);
        };
        redact_registered_path(task.working_directory, "[AGENT_WORKTREE]");
        for (const auto& project : projects_) {
            redact_registered_path(project.root, "[AGENT_PROJECT]");
        }
        redact_windows_absolute_paths(&event.text);
    }
    // Output continues while a provider waits for consent (for example stderr
    // from background model discovery). Only a decision or turn termination
    // relinquishes the pending request, not the output event's default state.
    if (!task.pending_approval_request_id.empty() && !event.approval_request
        && !event.terminal
        && (event.state == redclaw::protocol::AgentTaskStateV1::kRunning
            || event.state == redclaw::protocol::AgentTaskStateV1::kStarting)) {
        event.state = redclaw::protocol::AgentTaskStateV1::kAwaitingApproval;
    }
    task.state = event.state;
    if (event.approval_request) {
        ++metrics_.approval_request_total;
        task.pending_approval_request_id = event.request_id;
        task.pending_approval_started_ms = event.received_at_ms != 0
            ? event.received_at_ms : steady_time_ms();
    }
    const bool terminal = event.terminal || terminal_state(event.state);
    const bool turn_rejected = event.state == redclaw::protocol::AgentTaskStateV1::kPaused
        && event.event_kind == "turn_rejected";
    if (terminal || turn_rejected) {
        // A completed/interrupted provider has relinquished this approval.
        // Leaving its timer alive changes a terminal task to paused one minute
        // later and can reject an unrelated follow-up. No consent is granted.
        task.pending_approval_request_id.clear();
        task.pending_approval_started_ms = 0;
    }
    if (terminal) {
        if (event.state == redclaw::protocol::AgentTaskStateV1::kCompleted) {
            ++metrics_.task_complete_total;
        } else {
            ++metrics_.task_failed_total;
        }
    }
    append_event_locked(task, std::move(event));
    if (terminal && active_task_id_ == task.task_id) {
        active_task_id_.reset();
        start_next_pending_ = true;
    }
    if (turn_rejected && active_task_id_ == task.task_id) {
        active_task_id_.reset();
        start_next_pending_ = true;
    }
    trim_event_cache_locked();
    if (terminal) {
        persist_completed_metadata_locked();
    }
}

void RemoteAgentBroker::drain_provider_events() {
    for (int count = 0; count < 64; ++count) {
        AgentProviderEvent event;
        {
            std::lock_guard lock(provider_events_mutex_);
            if (provider_events_->empty()) break;
            event = provider_events_->take_front();
        }
        append_provider_event(std::move(event));
    }
}

void RemoteAgentBroker::append_event_locked(TaskRecord& task, AgentProviderEvent event) {
    if (capacity_failed_) return;
    const std::size_t bytes = event_size(event);
    trim_event_cache_locked(&task, bytes);
    if (bytes > config_.max_event_bytes_per_task || bytes > config_.max_total_event_bytes
        || task.event_bytes > config_.max_event_bytes_per_task - bytes
        || total_event_bytes_ > config_.max_total_event_bytes - bytes || task.events.size() >= 4096) {
        // Keep existing critical records. Reject the new operation explicitly
        // through a reserved snapshot, rather than growing an unbounded cache.
        capacity_failed_ = true;
        task.cache_exhausted = true;
        task.transport_gap_pending = true;
        task.state = redclaw::protocol::AgentTaskStateV1::kFailed;
        ++metrics_.task_failed_total;
        ++metrics_.gap_total;
        if (auto* provider = provider_locked(task.provider)) {
            std::string ignored;
            if (!task.pending_approval_request_id.empty()) {
                (void)provider->respond_to_approval(task.task_id, task.pending_approval_request_id,
                    redclaw::protocol::AgentApprovalDecisionV1::kReject, &ignored);
            }
            (void)provider->interrupt(task.task_id, &ignored);
        }
        task.pending_approval_request_id.clear();
        active_task_id_.reset();
        return;
    }
    if (event.error_code == "provider_output_gap") {
        ++metrics_.gap_total;
    }
    const std::uint64_t sequence = task.next_event_sequence++;
    task.event_bytes += bytes;
    total_event_bytes_ += bytes;
    ++metrics_.event_total;
    metrics_.cached_event_bytes_peak = std::max(
        metrics_.cached_event_bytes_peak, total_event_bytes_);
    task.events.push_back(TaskRecord::CachedEvent{
        .sequence = sequence,
        .bytes = bytes,
        .event = std::move(event),
    });
    if (connected_ && !task.replay_after_sequence) {
        const auto& cached = task.events.back();
        redclaw::protocol::AgentMessageEnvelopeV1 message;
        message.task_id = task.task_id;
        message.request_id = cached.event.request_id;
        message.event_sequence = cached.sequence;
        message.task_state = cached.event.state;
        message.event_kind = cached.event.event_kind;
        message.error_code = cached.event.error_code;
        message.text = cached.event.text;
        message.gap = cached.event.error_code == "provider_output_gap";
        message.type = cached.event.approval_request
            ? redclaw::protocol::AgentMessageTypeV1::kApprovalRequest
            : (cached.event.terminal
                ? (cached.event.state == redclaw::protocol::AgentTaskStateV1::kCompleted
                    ? redclaw::protocol::AgentMessageTypeV1::kTaskComplete
                    : redclaw::protocol::AgentMessageTypeV1::kTaskError)
                : redclaw::protocol::AgentMessageTypeV1::kEvent);
        enqueue_outbound_locked(std::move(message));
    }
}

void RemoteAgentBroker::start_next_queued_locked() {
    if (!connected_ || capacity_failed_ || active_task_id_.has_value()) {
        return;
    }
    if (!queued_turns_.empty()) {
        auto message = std::move(queued_turns_.front());
        queued_turns_.pop_front();
        const auto found = tasks_.find(message.task_id);
        if (found != tasks_.end()) {
            TaskRecord& task = *found->second;
            IAgentProvider* provider = provider_locked(task.provider);
            std::string error;
            active_task_id_ = task.task_id;
            task.state = redclaw::protocol::AgentTaskStateV1::kStarting;
            if (provider != nullptr
                && provider->start_turn(task.task_id, message.text, &error)) {
                return;
            }
            active_task_id_.reset();
            task.state = redclaw::protocol::AgentTaskStateV1::kFailed;
            append_event_locked(task, AgentProviderEvent{
                .task_id = task.task_id,
                .event_kind = "turn_error",
                .text = error,
                .error_code = "turn_start_failed",
                .state = redclaw::protocol::AgentTaskStateV1::kFailed,
                .terminal = true,
            });
        }
    }
    for (const auto& task_id : task_order_) {
        TaskRecord& task = *tasks_.at(task_id);
        if (task.state != redclaw::protocol::AgentTaskStateV1::kQueued) {
            continue;
        }
        redclaw::protocol::AgentMessageEnvelopeV1 request;
        request.provider = task.provider;
        request.project_id = task.project_id;
        request.model = task.model;
        request.text = task.initial_instruction;
        request.work_directory_mode = task.mode;
        std::string error;
        if (!start_task_locked(task, request, &error)) {
            task.state = redclaw::protocol::AgentTaskStateV1::kFailed;
            append_event_locked(task, AgentProviderEvent{
                .task_id = task.task_id,
                .event_kind = "task_error",
                .text = error,
                .error_code = "start_failed",
                .state = redclaw::protocol::AgentTaskStateV1::kFailed,
                .terminal = true,
            });
            continue;
        }
        break;
    }
}

void RemoteAgentBroker::publish_snapshot_locked(
    TaskRecord& task,
    std::uint64_t after_sequence) {
    // Keep one cursor, not a second copy of the entire retained transcript.
    // A replay larger than the transport queue must not evict its own prefix.
    if (!task.replay_after_sequence) task.replay_after_sequence = after_sequence;
}

void RemoteAgentBroker::pump_snapshot_locked(TaskRecord& task, std::size_t limit) {
    if (!task.replay_after_sequence || limit == 0) return;
    auto& after_sequence = *task.replay_after_sequence;
    redclaw::protocol::AgentMessageEnvelopeV1 snapshot;
    snapshot.type = redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot;
    snapshot.task_id = task.task_id;
    snapshot.provider = task.provider;
    snapshot.model = task.model;
    snapshot.project_id = task.project_id;
    snapshot.work_directory_mode = task.mode;
    snapshot.task_state = task.state;
    if (task.cache_exhausted) {
        snapshot.error_code = "agent_cache_exhausted";
        snapshot.text = "Agent stopped: critical event capacity exhausted; inspect task before retrying";
        snapshot.gap = after_sequence < task.next_event_sequence - 1;
    }
    snapshot.gap = snapshot.gap || (task.cache_gap && after_sequence < task.evicted_through_sequence);
    if (snapshot.gap) {
        ++metrics_.gap_total;
    }
    if (snapshot.gap) {
        // Persist the missing-history boundary before replaying its tail.
        // Sequence zero would request the same unfillable tail forever.
        auto gap = snapshot;
        gap.event_sequence = task.cache_exhausted
            ? task.next_event_sequence - 1 : task.evicted_through_sequence;
        gap.event_kind = "history_gap";
        if (gap.error_code.empty()) {
            gap.error_code = "history_unavailable";
            gap.text = "Earlier Agent output is unavailable. Retained task state follows; missing text was not delivered.";
        }
        after_sequence = std::max(after_sequence, gap.event_sequence);
        enqueue_outbound_locked(std::move(gap));
        if (--limit == 0) return;
    }
    for (const auto& cached : task.events) {
        if (cached.sequence <= after_sequence) {
            continue;
        }
        ++metrics_.replayed_event_total;
        redclaw::protocol::AgentMessageEnvelopeV1 message;
        const bool pending_approval = cached.event.approval_request
            && task.state == redclaw::protocol::AgentTaskStateV1::kAwaitingApproval
            && cached.event.request_id == task.pending_approval_request_id;
        message.type = pending_approval
            ? redclaw::protocol::AgentMessageTypeV1::kApprovalRequest
            : (cached.event.terminal
                ? (cached.event.state == redclaw::protocol::AgentTaskStateV1::kCompleted
                    ? redclaw::protocol::AgentMessageTypeV1::kTaskComplete
                    : redclaw::protocol::AgentMessageTypeV1::kTaskError)
                : redclaw::protocol::AgentMessageTypeV1::kEvent);
        message.task_id = task.task_id;
        message.request_id = cached.event.request_id;
        message.event_sequence = cached.sequence;
        message.task_state = cached.event.state;
        message.event_kind = cached.event.event_kind;
        if (cached.event.approval_request && !pending_approval) message.event_kind = "approval_history";
        message.error_code = cached.event.error_code;
        message.text = cached.event.text;
        message.gap = cached.event.error_code == "provider_output_gap";
        enqueue_outbound_locked(std::move(message));
        after_sequence = cached.sequence;
        if (--limit == 0) return;
    }
    // Current state follows history, so replay cannot leave a finished task running.
    snapshot.gap = false;
    snapshot.event_sequence = task.next_event_sequence - 1;
    if (task.state == redclaw::protocol::AgentTaskStateV1::kAwaitingApproval
        && !task.pending_approval_request_id.empty()) {
        const auto pending = std::find_if(task.events.begin(), task.events.end(), [&](const auto& cached) {
            return cached.event.approval_request && cached.event.request_id == task.pending_approval_request_id;
        });
        if (pending != task.events.end()) {
            snapshot.request_id = task.pending_approval_request_id;
            snapshot.event_kind = "approval_pending";
            snapshot.text = pending->event.text;
        }
    }
    enqueue_outbound_locked(std::move(snapshot));
    task.replay_after_sequence.reset();
}

void RemoteAgentBroker::enqueue_outbound_locked(
    redclaw::protocol::AgentMessageEnvelopeV1 message) {
    message.session_epoch = session_epoch_;
    message.message_id = next_message_id_++;
    message.sent_at_ms = unix_time_ms();
    if (outbound_.size() >= config_.max_outbound_messages) {
        const auto droppable = std::find_if(
            outbound_.begin(), outbound_.end(), [](const auto& candidate) {
                return candidate.type == redclaw::protocol::AgentMessageTypeV1::kEvent;
            });
        if (droppable != outbound_.end()) {
            if (const auto task = tasks_.find(droppable->task_id); task != tasks_.end()) {
                publish_snapshot_locked(*task->second, task->second->acknowledged_event_sequence);
            }
            outbound_.erase(droppable);
        } else {
            if (const auto task = tasks_.find(message.task_id); task != tasks_.end()) {
                publish_snapshot_locked(*task->second, task->second->acknowledged_event_sequence);
            }
            return;
        }
    }
    outbound_.push_back(std::move(message));
    metrics_.outbound_queue_peak = std::max(
        metrics_.outbound_queue_peak, outbound_.size());
}

std::vector<redclaw::protocol::AgentMessageEnvelopeV1> RemoteAgentBroker::take_outbound(
    std::size_t max_messages) {
    drain_provider_events();
    std::lock_guard lock(mutex_);
    if (start_next_pending_) {
        start_next_pending_ = false;
        start_next_queued_locked();
    }
    std::vector<redclaw::protocol::AgentMessageEnvelopeV1> messages;
    while (!outbound_.empty() && messages.size() < max_messages) {
        messages.push_back(std::move(outbound_.front()));
        outbound_.pop_front();
    }
    for (const auto& task_id : task_order_) {
        if (messages.size() >= max_messages) break;
        pump_snapshot_locked(*tasks_.at(task_id), std::min(
            max_messages - messages.size(), config_.max_outbound_messages - outbound_.size()));
        while (!outbound_.empty() && messages.size() < max_messages) {
            messages.push_back(std::move(outbound_.front()));
            outbound_.pop_front();
        }
    }
    if (messages.size() < max_messages) {
        for (const auto& task_id : task_order_) {
            TaskRecord& task = *tasks_.at(task_id);
            if (!task.transport_gap_pending) {
                continue;
            }
            redclaw::protocol::AgentMessageEnvelopeV1 gap;
            gap.type = redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot;
            gap.task_id = task.task_id;
            gap.provider = task.provider;
            gap.model = task.model;
            gap.project_id = task.project_id;
            gap.work_directory_mode = task.mode;
            gap.task_state = task.state;
            if (task.cache_exhausted) {
                gap.error_code = "agent_cache_exhausted";
                gap.text = "Agent stopped: critical event capacity exhausted; inspect task before retrying";
            }
            gap.gap = true;
            gap.event_sequence = task.next_event_sequence > 0
                ? task.next_event_sequence - 1U : 0U;
            gap.session_epoch = session_epoch_;
            gap.message_id = next_message_id_++;
            gap.sent_at_ms = unix_time_ms();
            messages.push_back(std::move(gap));
            task.transport_gap_pending = false;
            ++metrics_.gap_total;
            if (messages.size() >= max_messages) {
                break;
            }
        }
    }
    return messages;
}

std::size_t RemoteAgentBroker::queued_turn_count() const {
    std::lock_guard lock(mutex_);
    return queued_turns_.size() + static_cast<std::size_t>(std::count_if(
        tasks_.begin(), tasks_.end(), [](const auto& pair) {
            return pair.second->state == redclaw::protocol::AgentTaskStateV1::kQueued;
        }));
}

std::size_t RemoteAgentBroker::cached_event_bytes() const {
    std::lock_guard lock(mutex_);
    return total_event_bytes_;
}

std::size_t RemoteAgentBroker::queued_outbound_count() const {
    std::lock_guard lock(mutex_);
    return outbound_.size();
}

RemoteAgentBrokerMetrics RemoteAgentBroker::metrics() const {
    std::lock_guard lock(mutex_);
    return metrics_;
}

void RemoteAgentBroker::load_completed_metadata_locked() {
    if (config_.metadata_path.empty()) {
        return;
    }
    std::error_code size_error;
    const auto bytes = std::filesystem::file_size(config_.metadata_path, size_error);
    constexpr auto maximum_file_bytes = 20ULL * (redclaw::protocol::kMaxLocalRuntimeAgentFrameBytes + 2ULL);
    if (!size_error && bytes > maximum_file_bytes) {
        metadata_write_blocked_ = true;
        return;
    }
    std::ifstream input(config_.metadata_path, std::ios::binary);
    if (!input.is_open()) {
        return;
    }
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto parsed = detail::parse_completed_agent_metadata(line);
        if (!parsed.ok) {
            // Never overwrite an unreadable/unknown-version history file with
            // an empty/new history after upgrade. Preserve it for local recovery.
            metadata_write_blocked_ = true;
            continue;
        }
        if (parsed.value.type != redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot
            || !terminal_state(parsed.value.task_state)
            || tasks_.contains(parsed.value.task_id)) {
            continue;
        }
        auto task = std::make_unique<TaskRecord>();
        task->task_id = parsed.value.task_id;
        task->provider = parsed.value.provider;
        task->model = parsed.value.model;
        task->project_id = parsed.value.project_id;
        task->mode = parsed.value.work_directory_mode;
        task->state = parsed.value.task_state;
        task->next_event_sequence = std::max<std::uint64_t>(
            1U, parsed.value.event_sequence + 1U);
        // Only metadata survived the process lifetime. Make the lost history
        // explicit before publishing its terminal watermark, not a fake replay.
        task->evicted_through_sequence = task->next_event_sequence - 1;
        task->cache_gap = task->evicted_through_sequence != 0;
        task_order_.push_back(task->task_id);
        tasks_.emplace(task->task_id, std::move(task));
        while (task_order_.size() > config_.max_tasks) {
            tasks_.erase(task_order_.front());
            task_order_.pop_front();
        }
    }
}

void RemoteAgentBroker::persist_completed_metadata_locked() {
    if (config_.metadata_path.empty() || metadata_write_blocked_) {
        return;
    }
    std::error_code filesystem_error;
    const auto parent = config_.metadata_path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            return;
        }
    }
    std::filesystem::path temporary = config_.metadata_path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return;
    }
    std::uint64_t message_id = 0;
    for (const auto& task_id : task_order_) {
        const TaskRecord& task = *tasks_.at(task_id);
        if (!terminal_state(task.state)) {
            continue;
        }
        redclaw::protocol::AgentMessageEnvelopeV1 snapshot;
        snapshot.session_epoch = "persisted-agent-metadata";
        snapshot.message_id = ++message_id;
        snapshot.sent_at_ms = unix_time_ms();
        snapshot.type = redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot;
        snapshot.task_id = task.task_id;
        snapshot.provider = task.provider;
        snapshot.model = task.model;
        snapshot.project_id = task.project_id;
        snapshot.work_directory_mode = task.mode;
        snapshot.task_state = task.state;
        snapshot.event_sequence = task.next_event_sequence > 0
            ? task.next_event_sequence - 1U : 0U;
        output << redclaw::protocol::serialize_local_runtime_agent_frame_v1(snapshot) << '\n';
    }
    output.flush();
    if (!output.good()) {
        output.close();
        std::filesystem::remove(temporary, filesystem_error);
        return;
    }
    output.close();
#ifdef _WIN32
    if (MoveFileExW(
            temporary.wstring().c_str(), config_.metadata_path.wstring().c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        std::filesystem::remove(temporary, filesystem_error);
    }
#else
    std::filesystem::rename(temporary, config_.metadata_path, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary, filesystem_error);
    }
#endif
}

void RemoteAgentBroker::trim_event_cache_locked(TaskRecord* reserve_task, std::size_t reserve_bytes) {
    const auto remove_event = [&](TaskRecord& task, auto iterator) {
        task.evicted_through_sequence = std::max(
            task.evicted_through_sequence, iterator->sequence);
        task.event_bytes -= iterator->bytes;
        total_event_bytes_ -= iterator->bytes;
        task.events.erase(iterator);
        task.cache_gap = true;
    };
    for (auto& [task_id, task_ptr] : tasks_) {
        (void)task_id;
        TaskRecord& task = *task_ptr;
        const auto limit = config_.max_event_bytes_per_task - (reserve_task == &task
            ? std::min(reserve_bytes, config_.max_event_bytes_per_task) : 0U);
        while ((task.event_bytes > limit || task.events.size() >= 4096) && !task.events.empty()) {
            auto candidate = std::find_if(task.events.begin(), task.events.end(),
                [](const TaskRecord::CachedEvent& event) { return event.event.text_delta; });
            if (candidate == task.events.end()) {
                candidate = std::find_if(task.events.begin(), task.events.end(),
                    [](const TaskRecord::CachedEvent& event) {
                        return !event.event.approval_request && !event.event.terminal
                            && event.event.error_code.empty();
                    });
            }
            if (candidate == task.events.end()) {
                break;
            }
            remove_event(task, candidate);
        }
    }
    while (total_event_bytes_ > config_.max_total_event_bytes
            - std::min(reserve_bytes, config_.max_total_event_bytes)) {
        TaskRecord* candidate_task = nullptr;
        for (const auto& task_id : task_order_) {
            TaskRecord& task = *tasks_.at(task_id);
            if (std::any_of(task.events.begin(), task.events.end(),
                    [](const TaskRecord::CachedEvent& event) { return event.event.text_delta; })) {
                candidate_task = &task;
                break;
            }
        }
        if (candidate_task == nullptr) {
            break;
        }
        auto candidate = std::find_if(candidate_task->events.begin(), candidate_task->events.end(),
            [](const TaskRecord::CachedEvent& event) { return event.event.text_delta; });
        remove_event(*candidate_task, candidate);
    }
    while (task_order_.size() > config_.max_tasks) {
        const std::string oldest = task_order_.front();
        if (active_task_id_ == oldest || !terminal_state(tasks_.at(oldest)->state)) {
            break;
        }
        total_event_bytes_ -= tasks_.at(oldest)->event_bytes;
        tasks_.erase(oldest);
        task_order_.pop_front();
    }
}

IAgentProvider* RemoteAgentBroker::provider_locked(
    redclaw::protocol::AgentProviderKindV1 provider) {
    for (auto& candidate : providers_) {
        if (candidate->probe().provider == provider) {
            return candidate.get();
        }
    }
    return nullptr;
}

bool load_agent_project_manifest(
    const std::filesystem::path& manifest_path,
    std::vector<AgentProjectRegistration>* projects,
    std::string* error) {
    if (projects == nullptr) {
        assign_error("project output must be non-null", error);
        return false;
    }
    std::ifstream input(manifest_path, std::ios::binary);
    if (!input.is_open()) {
        assign_error("failed to open agent project manifest", error);
        return false;
    }
    std::vector<AgentProjectRegistration> parsed;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t first = line.find('|');
        const std::size_t second = first == std::string::npos
            ? std::string::npos : line.find('|', first + 1U);
        if (first == std::string::npos || second == std::string::npos) {
            assign_error("invalid agent project manifest line " + std::to_string(line_number), error);
            return false;
        }
        AgentProjectRegistration project;
        project.project_id = line.substr(0, first);
        project.display_name = line.substr(first + 1U, second - first - 1U);
        project.root = std::filesystem::path(line.substr(second + 1U));
        std::error_code filesystem_error;
        project.root = std::filesystem::weakly_canonical(project.root, filesystem_error);
        if (!safe_id(project.project_id) || project.display_name.empty()
            || project.display_name.size() > 256U || filesystem_error
            || !project.root.is_absolute()
            || !std::filesystem::is_directory(project.root, filesystem_error)) {
            assign_error("invalid agent project registration at line "
                + std::to_string(line_number), error);
            return false;
        }
        project.git_repository = std::filesystem::is_directory(project.root / ".git", filesystem_error)
            || std::filesystem::is_regular_file(project.root / ".git", filesystem_error);
        parsed.push_back(std::move(project));
    }
    *projects = std::move(parsed);
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

}  // namespace redclaw::agent
