#include "redclaw/agent/agent_providers.h"
#include "redclaw/agent/provider_dispatch_queue.h"
#include <atomic>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <boost/json.hpp>
#include <boost/system/error_code.hpp>

namespace redclaw::agent {
namespace {

void assign_error(std::string value, std::string* error) {
    if (error != nullptr) {
        *error = std::move(value);
    }
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}

std::optional<std::string> json_string(
    const boost::json::object& object,
    std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->value().is_string()) {
        return std::nullopt;
    }
    return std::string(found->value().as_string());
}

std::optional<std::string> nested_string(
    const boost::json::object& object,
    std::string_view object_key,
    std::string_view key) {
    const auto found = object.find(object_key);
    if (found == object.end() || !found->value().is_object()) {
        return std::nullopt;
    }
    return json_string(found->value().as_object(), key);
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool reports_not_authenticated(const std::string& output) {
    const std::string normalized = lowercase(output);
    return normalized.find("not logged") != std::string::npos
        || normalized.find("not authenticated") != std::string::npos
        || normalized.find("login required") != std::string::npos;
}

bool reports_unsupported_priority_service_tier(const std::string& output) {
    const std::string normalized = lowercase(output);
    return normalized.find("service_tier") != std::string::npos
        && normalized.find("unknown variant") != std::string::npos
        && normalized.find("priority") != std::string::npos;
}

std::vector<std::string> codex_arguments(
    bool compatibility_override,
    std::initializer_list<std::string_view> arguments) {
    std::vector<std::string> result;
    if (compatibility_override) {
        result.emplace_back("-c");
        result.emplace_back("service_tier=\"fast\"");
    }
    for (const auto argument : arguments) {
        result.emplace_back(argument);
    }
    return result;
}

class CodexAppServerProvider final : public IAgentProvider {
public:
    explicit CodexAppServerProvider(std::unique_ptr<IAgentProcess> process)
        : process_(std::move(process)), dispatch_thread_([this]() { dispatch_events(); }) {}

    ~CodexAppServerProvider() override {
        shutdown();
    }

    AgentProviderProbe probe(bool force_refresh) override {
        std::lock_guard lock(mutex_);
        if (force_refresh) {
            probe_complete_ = false;
        }
        if (!probe_complete_) {
            refresh_probe_locked();
        }
        return {
            .provider = redclaw::protocol::AgentProviderKindV1::kCodex,
            .available = available_,
            .readiness = readiness_,
            .version = version_,
            .models = {},
            .supports_structured_approval = true,
            .requires_turn_approval = false,
            .unavailable_reason = unavailable_reason_,
        };
    }

    void set_event_sink(AgentProviderEventSink sink) override {
        std::lock_guard lock(mutex_);
        sink_ = std::move(sink);
    }

    bool start_task(const AgentProviderTaskRequest& request, std::string* error) override {
        std::unique_lock lock(mutex_);
        if (!ensure_process_locked(request.working_directory, lock, error)) {
            return false;
        }
        if (!current_task_id_.empty()) {
            assign_error("Codex provider already owns an active RedClaw task", error);
            return false;
        }
        current_task_id_ = request.task_id;
        thread_id_.clear();
        turn_id_.clear();
        model_ = request.model;
        working_directory_ = request.working_directory;
        pending_instruction_ = request.instruction;
        if (!initialized_) {
            begin_after_initialize_ = BeginAfterInitialize::kStart;
            return true;
        }
        if (!send_thread_start_locked(error)) {
            current_task_id_.clear();
            return false;
        }
        return true;
    }

    bool resume_task(const AgentProviderTaskRequest& request, std::string* error) override {
        std::unique_lock lock(mutex_);
        const auto registered = thread_ids_.find(request.task_id);
        if (registered == thread_ids_.end()) {
            assign_error("Codex thread is not registered for this RedClaw task", error);
            return false;
        }
        const auto registered_thread = registered->second;
        if (!ensure_process_locked(request.working_directory, lock, error)) {
            return false;
        }
        current_task_id_ = request.task_id;
        thread_id_ = registered_thread;
        pending_instruction_ = request.instruction;
        if (!initialized_) {
            begin_after_initialize_ = BeginAfterInitialize::kResume;
            return true;
        }
        return send_thread_resume_locked(error);
    }

    bool send_thread_resume_locked(std::string* error) {
        const std::uint64_t request_id = next_rpc_id_++;
        pending_requests_[request_id] = PendingRequest::kThreadResume;
        boost::json::object params;
        params["threadId"] = thread_id_;
        return send_request_locked(request_id, "thread/resume", std::move(params), error);
    }

    bool start_turn(
        const std::string& task_id,
        const std::string& instruction,
        std::string* error) override {
        std::lock_guard lock(mutex_);
        if (!current_task_id_.empty() || !turn_id_.empty()) {
            assign_error("Codex task is not ready for a new turn", error);
            return false;
        }
        const auto registered = thread_ids_.find(task_id);
        if (registered == thread_ids_.end()) {
            assign_error("Codex thread is not registered for this RedClaw task", error);
            return false;
        }
        current_task_id_ = task_id;
        thread_id_ = registered->second;
        return send_turn_locked(instruction, error);
    }

    bool steer(
        const std::string& task_id,
        const std::string& instruction,
        std::string* error) override {
        std::lock_guard lock(mutex_);
        if (task_id != current_task_id_ || thread_id_.empty() || turn_id_.empty()) {
            assign_error("Codex turn is not active", error);
            return false;
        }
        boost::json::object params;
        params["threadId"] = thread_id_;
        params["turnId"] = turn_id_;
        params["input"] = make_text_input(instruction);
        const std::uint64_t request_id = next_rpc_id_++;
        pending_requests_[request_id] = PendingRequest::kSteer;
        return send_request_locked(request_id, "turn/steer", std::move(params), error);
    }

    bool interrupt(const std::string& task_id, std::string* error) override {
        std::lock_guard lock(mutex_);
        if (task_id != current_task_id_ || thread_id_.empty() || turn_id_.empty()) {
            assign_error("Codex turn is not active", error);
            return false;
        }
        boost::json::object params;
        params["threadId"] = thread_id_;
        params["turnId"] = turn_id_;
        const std::uint64_t request_id = next_rpc_id_++;
        pending_requests_[request_id] = PendingRequest::kInterrupt;
        return send_request_locked(request_id, "turn/interrupt", std::move(params), error);
    }

    bool respond_to_approval(
        const std::string& task_id,
        const std::string& request_id,
        redclaw::protocol::AgentApprovalDecisionV1 decision,
        std::string* error) override {
        std::lock_guard lock(mutex_);
        if (task_id != current_task_id_) {
            assign_error("approval does not belong to active Codex task", error);
            return false;
        }
        const auto found = approval_rpc_ids_.find(request_id);
        if (found == approval_rpc_ids_.end()) {
            assign_error("unknown Codex approval request", error);
            return false;
        }
        if (decision == redclaw::protocol::AgentApprovalDecisionV1::kAccept
            && undescribed_approvals_.contains(request_id)) {
            assign_error("approval lacks an auditable operation description", error);
            return false;
        }
        boost::json::object result;
        result["decision"] = decision == redclaw::protocol::AgentApprovalDecisionV1::kAccept
            ? "accept" : "decline";
        boost::json::object response;
        response["id"] = found->second;
        response["result"] = std::move(result);
        if (!process_->write_line(boost::json::serialize(response), error)) return false;
        approval_rpc_ids_.erase(found);
        undescribed_approvals_.erase(request_id);
        return true;
    }

    void shutdown() override {
        std::unique_ptr<IAgentProcess>* process = nullptr;
        {
            std::lock_guard lock(mutex_);
            if (shutting_down_) {
                return;
            }
            shutting_down_ = true;
            process = &process_;
        }
        if (*process != nullptr) {
            (*process)->stop();
        }
        pending_events_.stop();
        if (dispatch_thread_.joinable()
            && dispatch_thread_.get_id() != std::this_thread::get_id()) {
            dispatch_thread_.join();
        }
    }

private:
    enum class PendingRequest {
        kInitialize,
        kThreadStart,
        kThreadResume,
        kTurnStart,
        kSteer,
        kInterrupt,
    };
    enum class BeginAfterInitialize { kNone, kStart, kResume };

    static boost::json::array make_text_input(const std::string& instruction) {
        boost::json::object item;
        item["type"] = "text";
        item["text"] = instruction;
        boost::json::array input;
        input.push_back(std::move(item));
        return input;
    }

    bool ensure_process_locked(
        const std::filesystem::path& working_directory,
        std::unique_lock<std::mutex>& lock,
        std::string* error) {
        if (pending_events_.failed()) {
            assign_error("Agent provider event capacity exhausted", error);
            return false;
        }
        if (process_running_) {
            return initialized_;
        }
        if (!probe_complete_) {
            refresh_probe_locked();
        }
        if (!available_) {
            assign_error(unavailable_reason_, error);
            return false;
        }
        const auto launch_arguments = codex_arguments(
            use_service_tier_compatibility_override_, {"app-server", "--listen", "stdio://"});
        lock.unlock();
        const auto generation = ++process_generation_;
        const bool started = process_->start(
            "codex", launch_arguments, working_directory,
            [this, generation](std::string line) { pending_events_.push_process({ProviderProcessEvent::Kind::kStdout, std::move(line), 0, generation}); },
            [this, generation](std::string line) { pending_events_.push_process({ProviderProcessEvent::Kind::kStderr, std::move(line), 0, generation}); },
            [this, generation](int exit_code) { pending_events_.push_process({ProviderProcessEvent::Kind::kExit, {}, exit_code, generation}); }, error);
        lock.lock();
        process_running_ = started;
        if (!process_running_) {
            return false;
        }
        const std::uint64_t request_id = next_rpc_id_++;
        pending_requests_[request_id] = PendingRequest::kInitialize;
        boost::json::object client_info;
        client_info["name"] = "redclaw-desktop";
        client_info["title"] = "RedClaw Desktop";
        client_info["version"] = "1";
        boost::json::object params;
        params["clientInfo"] = std::move(client_info);
        if (!send_request_locked(request_id, "initialize", std::move(params), error)) {
            process_running_ = false;
            lock.unlock();
            process_->stop();
            lock.lock();
            return false;
        }
        initialized_ = false;
        return true;
    }

    void refresh_probe_locked() {
        available_ = false;
        version_.clear();
        unavailable_reason_.clear();
        use_service_tier_compatibility_override_ = false;
        readiness_ = redclaw::protocol::AgentProviderReadinessV1::kProbeFailed;
        std::string executable_path;
        if (process_ == nullptr
            || !process_->executable_available("codex", &executable_path)) {
            readiness_ = redclaw::protocol::AgentProviderReadinessV1::kCliMissing;
            unavailable_reason_ = "codex CLI is not installed";
            probe_complete_ = true;
            return;
        }

        int version_exit = 1;
        int status_exit = 1;
        int app_server_exit = 1;
        std::string version_output;
        std::string status_output;
        std::string app_server_output;
        std::string ignored;
        const bool version_ok = process_->run_probe(
            "codex", {"--version"}, 5000, &version_exit, &version_output, &ignored)
            && version_exit == 0;
        version_ = version_ok ? version_output : std::string{};
        bool status_ran = process_->run_probe(
            "codex", {"login", "status"}, 10000,
            &status_exit, &status_output, &ignored);
        if ((!status_ran || status_exit != 0)
            && reports_unsupported_priority_service_tier(status_output)) {
            status_output.clear();
            ignored.clear();
            status_ran = process_->run_probe(
                "codex", codex_arguments(true, {"login", "status"}), 10000,
                &status_exit, &status_output, &ignored);
            use_service_tier_compatibility_override_ = status_ran && status_exit == 0;
        }
        if (!status_ran || status_exit != 0) {
            readiness_ = reports_not_authenticated(status_output)
                ? redclaw::protocol::AgentProviderReadinessV1::kNotAuthenticated
                : redclaw::protocol::AgentProviderReadinessV1::kProbeFailed;
            unavailable_reason_ = readiness_
                    == redclaw::protocol::AgentProviderReadinessV1::kNotAuthenticated
                ? "codex account is not authenticated"
                : "codex login status probe failed";
            probe_complete_ = true;
            return;
        }
        const bool app_server_ok = process_->run_probe(
            "codex", codex_arguments(
                use_service_tier_compatibility_override_, {"app-server", "--help"}), 5000,
            &app_server_exit, &app_server_output, &ignored)
            && app_server_exit == 0;
        if (!version_ok || !app_server_ok) {
            unavailable_reason_ = "codex app-server readiness probe failed";
            probe_complete_ = true;
            return;
        }
        available_ = true;
        readiness_ = redclaw::protocol::AgentProviderReadinessV1::kReady;
        probe_complete_ = true;
    }

    bool send_thread_start_locked(std::string* error) {
        const std::uint64_t request_id = next_rpc_id_++;
        pending_requests_[request_id] = PendingRequest::kThreadStart;
        boost::json::object params;
        params["cwd"] = path_utf8(working_directory_);
        // The current app-server schema calls the former unless-trusted
        // behavior "untrusted".  Use the protocol token, not a CLI spelling.
        params["approvalPolicy"] = "untrusted";
        params["sandbox"] = "workspace-write";
        if (!model_.empty()) {
            params["model"] = model_;
        }
        return send_request_locked(request_id, "thread/start", std::move(params), error);
    }

    bool send_turn_locked(const std::string& instruction, std::string* error) {
        boost::json::object params;
        params["threadId"] = thread_id_;
        params["input"] = make_text_input(instruction);
        const std::uint64_t request_id = next_rpc_id_++;
        pending_requests_[request_id] = PendingRequest::kTurnStart;
        return send_request_locked(request_id, "turn/start", std::move(params), error);
    }

    bool send_request_locked(
        std::uint64_t request_id,
        std::string_view method,
        boost::json::object params,
        std::string* error) {
        boost::json::object request;
        request["id"] = request_id;
        request["method"] = method;
        request["params"] = std::move(params);
        return process_->write_line(boost::json::serialize(request), error);
    }

    void on_stdout(std::string line) {
        boost::system::error_code json_error;
        boost::json::value parsed = boost::json::parse(line, json_error);
        if (json_error || !parsed.is_object()) {
            emit_threadsafe({
                .task_id = current_task_id_copy(),
                .event_kind = "provider_protocol_error",
                .text = "Codex app-server emitted invalid JSON",
                .error_code = "invalid_json",
                .state = redclaw::protocol::AgentTaskStateV1::kFailed,
                .terminal = true,
            });
            return;
        }
        std::lock_guard lock(mutex_);
        const auto& object = parsed.as_object();
        if (object.contains("id") && (object.contains("result") || object.contains("error"))) {
            handle_response_locked(object);
            return;
        }
        const auto method = json_string(object, "method");
        if (!method.has_value()) {
            return;
        }
        const auto params_found = object.find("params");
        const boost::json::object empty;
        const auto& params = params_found != object.end() && params_found->value().is_object()
            ? params_found->value().as_object() : empty;
        handle_notification_locked(*method, params, object);
    }

    void handle_response_locked(const boost::json::object& object) {
        const auto id_found = object.find("id");
        if (id_found == object.end()
            || (!id_found->value().is_uint64() && !id_found->value().is_int64())) {
            return;
        }
        const std::uint64_t id = id_found->value().is_uint64()
            ? id_found->value().as_uint64()
            : (id_found->value().as_int64() >= 0
                ? static_cast<std::uint64_t>(id_found->value().as_int64()) : 0U);
        const auto pending = pending_requests_.find(id);
        if (pending == pending_requests_.end()) {
            return;
        }
        const PendingRequest kind = pending->second;
        pending_requests_.erase(pending);
        const auto error_found = object.find("error");
        if (error_found != object.end()) {
            emit_locked({
                .task_id = current_task_id_,
                .event_kind = "provider_error",
                .text = "Codex app-server request failed",
                .error_code = "rpc_error",
                .state = redclaw::protocol::AgentTaskStateV1::kFailed,
                .terminal = true,
            });
            return;
        }
        const auto result_found = object.find("result");
        if (result_found == object.end() || !result_found->value().is_object()) {
            return;
        }
        const auto& result = result_found->value().as_object();
        if (kind == PendingRequest::kInitialize) {
            initialized_ = true;
            boost::json::object initialized;
            initialized["method"] = "initialized";
            initialized["params"] = boost::json::object{};
            std::string error;
            if (!process_->write_line(boost::json::serialize(initialized), &error)) {
                return;
            }
            const auto begin = std::exchange(
                begin_after_initialize_, BeginAfterInitialize::kNone);
            if (begin == BeginAfterInitialize::kStart) {
                (void)send_thread_start_locked(&error);
            } else if (begin == BeginAfterInitialize::kResume) {
                (void)send_thread_resume_locked(&error);
            }
        } else if (kind == PendingRequest::kThreadStart || kind == PendingRequest::kThreadResume) {
            thread_id_ = nested_string(result, "thread", "id")
                .value_or(json_string(result, "threadId").value_or(std::string{}));
            if (thread_id_.empty()) {
                emit_locked({
                    .task_id = current_task_id_,
                    .event_kind = "provider_protocol_error",
                    .text = "Codex app-server did not return a thread id",
                    .error_code = "missing_thread_id",
                    .state = redclaw::protocol::AgentTaskStateV1::kFailed,
                    .terminal = true,
                });
                return;
            }
            thread_ids_[current_task_id_] = thread_id_;
            std::string error;
            if (!send_turn_locked(std::exchange(pending_instruction_, {}), &error)) {
                emit_locked({
                    .task_id = current_task_id_,
                    .event_kind = "provider_error",
                    .text = "Failed to start Codex turn",
                    .error_code = "turn_start_failed",
                    .state = redclaw::protocol::AgentTaskStateV1::kFailed,
                    .terminal = true,
                });
            }
        } else if (kind == PendingRequest::kTurnStart) {
            turn_id_ = nested_string(result, "turn", "id")
                .value_or(json_string(result, "turnId").value_or(std::string{}));
        }
    }

    void handle_notification_locked(
        const std::string& method,
        const boost::json::object& params,
        const boost::json::object& full_object) {
        if (method.find("requestApproval") != std::string::npos) {
            const auto id_found = full_object.find("id");
            if (id_found == full_object.end()) {
                return;
            }
            const std::string request_id = "codex-approval-"
                + std::to_string(next_approval_id_++);
            approval_rpc_ids_[request_id] = id_found->value();
            std::string summary = "Approval operation: " + method;
            bool described = false;
            for (const auto* key : {"command", "reason", "cwd"}) {
                if (const auto value = json_string(params, key); value && !value->empty()) {
                    summary += "\n" + std::string(key) + ": " + value->substr(0, 1536);
                    if (std::string_view(key) != "cwd") described = true;
                }
            }
            if (!described) {
                summary += "\nOperation details unavailable. Reject and request an explicit scoped operation.";
                undescribed_approvals_.insert(request_id);
            }
            emit_locked({
                .task_id = current_task_id_,
                .request_id = request_id,
                .event_kind = method,
                .text = std::move(summary),
                .state = redclaw::protocol::AgentTaskStateV1::kAwaitingApproval,
                .approval_request = true,
            });
            return;
        }
        if (method == "turn/started") {
            turn_id_ = nested_string(params, "turn", "id")
                .value_or(json_string(params, "turnId").value_or(turn_id_));
            emit_locked({
                .task_id = current_task_id_,
                .event_kind = "turn_started",
                .state = redclaw::protocol::AgentTaskStateV1::kRunning,
            });
            return;
        }
        if (method == "turn/completed") {
            turn_id_.clear();
            const std::string completed_task_id = current_task_id_;
            emit_locked({
                .task_id = completed_task_id,
                .event_kind = "turn_completed",
                .text = "Codex turn completed",
                .state = redclaw::protocol::AgentTaskStateV1::kCompleted,
                .terminal = true,
            });
            current_task_id_.clear();
            thread_id_.clear();
            return;
        }
        if (method.find("agentMessage/delta") != std::string::npos
            || method.find("reasoning") != std::string::npos) {
            const std::string delta = json_string(params, "delta").value_or(std::string{});
            emit_text_chunks_locked(method, delta);
            return;
        }
        if (method.find("item/") != std::string::npos) {
            emit_locked({
                .task_id = current_task_id_,
                .event_kind = method,
                .state = redclaw::protocol::AgentTaskStateV1::kRunning,
            });
        }
    }

    void emit_text_chunks_locked(const std::string& kind, const std::string& text) {
        std::size_t offset = 0;
        do {
            const std::size_t count = std::min(
                redclaw::protocol::kMaxAgentEventChunkBytes, text.size() - offset);
            emit_locked({
                .task_id = current_task_id_,
                .event_kind = kind,
                .text = text.substr(offset, count),
                .state = redclaw::protocol::AgentTaskStateV1::kRunning,
                .text_delta = true,
            });
            offset += count;
        } while (offset < text.size());
    }

    void on_stderr(std::string line) {
        if (line.empty()) {
            return;
        }
        if (line.size() > redclaw::protocol::kMaxAgentEventChunkBytes) {
            line.resize(redclaw::protocol::kMaxAgentEventChunkBytes);
        }
        emit_threadsafe({
            .task_id = current_task_id_copy(),
            .event_kind = "provider_stderr",
            .text = std::move(line),
            .state = redclaw::protocol::AgentTaskStateV1::kRunning,
            .text_delta = true,
        });
    }

    void on_exit(int exit_code) {
        std::lock_guard lock(mutex_);
        process_running_ = false;
        initialized_ = false;
        if (!shutting_down_ && !current_task_id_.empty()) {
            emit_locked({
                .task_id = current_task_id_,
                .event_kind = "provider_exit",
                .text = "Codex app-server exited with code " + std::to_string(exit_code),
                .error_code = "provider_exit",
                .state = redclaw::protocol::AgentTaskStateV1::kFailed,
                .terminal = true,
            });
        }
    }

    std::string current_task_id_copy() {
        std::lock_guard lock(mutex_);
        return current_task_id_;
    }

    void emit_threadsafe(AgentProviderEvent event) {
        {
            std::lock_guard lock(mutex_);
            pending_events_.push_back(std::move(event));
        }
    }

    void emit_locked(AgentProviderEvent event) {
        pending_events_.push_back(std::move(event));
    }

    void dispatch_events() {
        while (auto item = pending_events_.take()) {
            if (auto* source = std::get_if<ProviderProcessEvent>(&*item)) {
                if (source->generation != process_generation_.load()) continue;
                switch (source->kind) {
                    case ProviderProcessEvent::Kind::kStdout: on_stdout(std::move(source->line)); break;
                    case ProviderProcessEvent::Kind::kStderr: on_stderr(std::move(source->line)); break;
                    case ProviderProcessEvent::Kind::kExit: on_exit(source->exit_code); break;
                    case ProviderProcessEvent::Kind::kOverflow:
                        on_exit(125); // Explicit failure; new operations are rejected by failed().
                        break;
                }
            } else {
                AgentProviderEventSink sink;
                { std::lock_guard lock(mutex_); sink = sink_; }
                if (sink) sink(std::move(std::get<AgentProviderEvent>(*item)));
            }
        }
    }

    std::unique_ptr<IAgentProcess> process_;
    std::mutex mutex_;
    ProviderDispatchQueue pending_events_;
    std::atomic<std::uint64_t> process_generation_{0};
    AgentProviderEventSink sink_;
    bool probe_complete_ = false;
    bool available_ = false;
    bool use_service_tier_compatibility_override_ = false;
    redclaw::protocol::AgentProviderReadinessV1 readiness_ =
        redclaw::protocol::AgentProviderReadinessV1::kProbeFailed;
    bool process_running_ = false;
    bool initialized_ = false;
    bool shutting_down_ = false;
    std::string version_;
    std::string unavailable_reason_;
    std::string current_task_id_;
    std::string model_;
    std::filesystem::path working_directory_;
    std::string pending_instruction_;
    std::string thread_id_;
    std::string turn_id_;
    std::uint64_t next_rpc_id_ = 1;
    std::uint64_t next_approval_id_ = 1;
    std::unordered_map<std::uint64_t, PendingRequest> pending_requests_;
    std::unordered_map<std::string, boost::json::value> approval_rpc_ids_;
    std::unordered_set<std::string> undescribed_approvals_;
    std::unordered_map<std::string, std::string> thread_ids_;
    BeginAfterInitialize begin_after_initialize_ = BeginAfterInitialize::kNone;
    std::thread dispatch_thread_;
};

}  // namespace

std::unique_ptr<IAgentProvider> make_codex_app_server_provider(
    std::unique_ptr<IAgentProcess> process) {
    return std::make_unique<CodexAppServerProvider>(std::move(process));
}

}  // namespace redclaw::agent
