#include "redclaw/agent/agent_providers.h"
#include "redclaw/agent/provider_dispatch_queue.h"
#include <atomic>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <sstream>
#include <mutex>
#include <thread>
#include <unordered_map>
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

std::optional<std::string> string_field(
    const boost::json::object& object,
    std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->value().is_string()) {
        return std::nullopt;
    }
    return std::string(found->value().as_string());
}

bool cursor_account_ready(std::string status) {
    std::transform(status.begin(), status.end(), status.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return status.find("not logged in") == std::string::npos
        && status.find("logged out") == std::string::npos
        && status.find("unauthenticated") == std::string::npos;
}

bool cursor_reports_not_authenticated(std::string status) {
    std::transform(status.begin(), status.end(), status.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return status.find("not logged") != std::string::npos
        || status.find("logged out") != std::string::npos
        || status.find("unauthenticated") != std::string::npos
        || status.find("login required") != std::string::npos;
}

std::vector<std::string> parse_grok_models(const std::string& output) {
    std::vector<std::string> models;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line) && models.size() < 32U) {
        std::istringstream tokens(line);
        std::string token;
        if (!(tokens >> token)) {
            continue;
        }
        if ((token == "-" || token == "*") && !(tokens >> token)) {
            continue;
        }
        while (!token.empty() && token.front() == '`') {
            token.erase(token.begin());
        }
        while (!token.empty()
               && (token.back() == ',' || token.back() == ':' || token.back() == '`')) {
            token.pop_back();
        }
        std::string normalized = token;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const bool valid = !token.empty() && token.size() <= 128U
            && std::all_of(token.begin(), token.end(), [](unsigned char ch) {
                return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '/';
            });
        if (valid && normalized.find("grok") != std::string::npos
            && std::find(models.begin(), models.end(), token) == models.end()) {
            models.push_back(std::move(token));
        }
    }
    return models;
}

class CursorAgentProvider final : public IAgentProvider {
public:
    explicit CursorAgentProvider(std::unique_ptr<IAgentProcess> process)
        : process_(std::move(process)), dispatch_thread_([this]() { dispatch_events(); }) {}

    ~CursorAgentProvider() override {
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
            .provider = redclaw::protocol::AgentProviderKindV1::kCursor,
            .available = available_,
            .readiness = readiness_,
            .version = version_,
            .models = models_,
            .supports_structured_approval = false,
            .requires_turn_approval = true,
            .unavailable_reason = unavailable_reason_,
        };
    }

    void set_event_sink(AgentProviderEventSink sink) override {
        std::lock_guard lock(mutex_);
        sink_ = std::move(sink);
    }

    bool start_task(const AgentProviderTaskRequest& request, std::string* error) override {
        std::lock_guard lock(mutex_);
        if (pending_events_.failed()) {
            assign_error("Agent event queue exhausted; synchronization required", error);
            return false;
        }
        if (!probe_locked().available) {
            assign_error(unavailable_reason_, error);
            return false;
        }
        if (request.model.empty()
            || std::find(models_.begin(), models_.end(), request.model) == models_.end()) {
            assign_error("requested Cursor model was not advertised by cursor-agent", error);
            return false;
        }
        if (!active_task_id_.empty() || pending_.has_value()) {
            assign_error("Cursor provider already owns an active turn", error);
            return false;
        }
        pending_ = PendingTurn{
            .request = request,
            .approval_request_id = "cursor-turn-" + std::to_string(next_approval_id_++),
        };
        emit_locked({
            .task_id = request.task_id,
            .request_id = pending_->approval_request_id,
            .event_kind = "cursor_turn_preapproval",
            .text = "Cursor Agent requests approval for the entire turn",
            .state = redclaw::protocol::AgentTaskStateV1::kAwaitingApproval,
            .approval_request = true,
        });
        return true;
    }

    bool resume_task(const AgentProviderTaskRequest& request, std::string* error) override {
        std::lock_guard lock(mutex_);
        if (pending_events_.failed()) {
            assign_error("Agent event queue exhausted; synchronization required", error);
            return false;
        }
        if (!probe_locked().available) {
            assign_error(unavailable_reason_, error);
            return false;
        }
        if (request.model.empty()
            || std::find(models_.begin(), models_.end(), request.model) == models_.end()) {
            assign_error("requested Cursor model was not advertised by cursor-agent", error);
            return false;
        }
        if (!chat_ids_.contains(request.task_id)) {
            assign_error("Cursor chat id is not registered for this RedClaw task", error);
            return false;
        }
        if (!active_task_id_.empty() || pending_.has_value()) {
            assign_error("Cursor provider already owns an active turn", error);
            return false;
        }
        pending_ = PendingTurn{
            .request = request,
            .approval_request_id = "cursor-turn-" + std::to_string(next_approval_id_++),
            .resume_chat_id = chat_ids_.at(request.task_id),
        };
        emit_locked({
            .task_id = request.task_id,
            .request_id = pending_->approval_request_id,
            .event_kind = "cursor_turn_preapproval",
            .text = "Cursor Agent requests approval for the entire resumed turn",
            .state = redclaw::protocol::AgentTaskStateV1::kAwaitingApproval,
            .approval_request = true,
        });
        return true;
    }

    bool start_turn(
        const std::string& task_id,
        const std::string& instruction,
        std::string* error) override {
        AgentProviderTaskRequest request;
        {
            std::lock_guard lock(mutex_);
            const auto found = chat_ids_.find(task_id);
            if (found == chat_ids_.end()) {
                assign_error("Cursor chat id is not registered for this RedClaw task", error);
                return false;
            }
            request = AgentProviderTaskRequest{
                .task_id = task_id,
                .model = task_models_[task_id],
                .working_directory = task_directories_[task_id],
                .instruction = instruction,
            };
        }
        return resume_task(request, error);
    }

    bool steer(const std::string&, const std::string&, std::string* error) override {
        assign_error("Cursor stream-json mode does not support mid-turn steering", error);
        return false;
    }

    bool interrupt(const std::string& task_id, std::string* error) override {
        {
            std::lock_guard lock(mutex_);
            if (task_id != active_task_id_ && (!pending_ || pending_->request.task_id != task_id)) {
                assign_error("Cursor task is not active", error);
                return false;
            }
            pending_.reset();
        }
        process_->stop();
        {
            std::lock_guard lock(mutex_);
            active_task_id_.clear();
            emit_locked({
                .task_id = task_id,
                .event_kind = "turn_interrupted",
                .state = redclaw::protocol::AgentTaskStateV1::kInterrupted,
                .terminal = true,
            });
        }
        return true;
    }

    bool respond_to_approval(
        const std::string& task_id,
        const std::string& request_id,
        redclaw::protocol::AgentApprovalDecisionV1 decision,
        std::string* error) override {
        PendingTurn turn;
        {
            std::lock_guard lock(mutex_);
            if (!pending_ || pending_->request.task_id != task_id
                || pending_->approval_request_id != request_id) {
                assign_error("Cursor turn approval is stale", error);
                return false;
            }
            turn = std::move(*pending_);
            pending_.reset();
            if (decision != redclaw::protocol::AgentApprovalDecisionV1::kAccept) {
                emit_locked({
                    .task_id = task_id,
                    .event_kind = "turn_rejected",
                    .state = redclaw::protocol::AgentTaskStateV1::kPaused,
                });
                return true;
            }
            active_task_id_ = task_id;
            task_models_[task_id] = turn.request.model;
            task_directories_[task_id] = turn.request.working_directory;
        }
        std::vector<std::string> arguments{
            "--print", "--output-format", "stream-json", "--trust", "--force",
        };
        if (!turn.request.model.empty()) {
            arguments.push_back("--model");
            arguments.push_back(turn.request.model);
        }
        if (!turn.resume_chat_id.empty()) {
            arguments.push_back("--resume");
            arguments.push_back(turn.resume_chat_id);
        }
        arguments.push_back(turn.request.instruction);
        const auto generation = ++process_generation_;
        if (!process_->start(
                "cursor-agent", arguments, turn.request.working_directory,
                [this, generation](std::string line) { pending_events_.push_process({ProviderProcessEvent::Kind::kStdout, std::move(line), 0, generation}); },
                [this, generation](std::string line) { pending_events_.push_process({ProviderProcessEvent::Kind::kStderr, std::move(line), 0, generation}); },
                [this, generation](int exit_code) { pending_events_.push_process({ProviderProcessEvent::Kind::kExit, {}, exit_code, generation}); }, error)) {
            std::lock_guard lock(mutex_);
            active_task_id_.clear();
            return false;
        }
        return true;
    }

    void shutdown() override {
        {
            std::lock_guard lock(mutex_);
            if (shutting_down_) {
                return;
            }
            shutting_down_ = true;
        }
        process_->stop();
        pending_events_.stop();
        if (dispatch_thread_.joinable()
            && dispatch_thread_.get_id() != std::this_thread::get_id()) {
            dispatch_thread_.join();
        }
    }

private:
    struct PendingTurn {
        AgentProviderTaskRequest request;
        std::string approval_request_id;
        std::string resume_chat_id;
    };

    AgentProviderProbe probe_locked() {
        if (!probe_complete_) {
            refresh_probe_locked();
        }
        return {
            .provider = redclaw::protocol::AgentProviderKindV1::kCursor,
            .available = available_,
            .readiness = readiness_,
            .version = version_,
            .models = models_,
            .supports_structured_approval = false,
            .requires_turn_approval = true,
            .unavailable_reason = unavailable_reason_,
        };
    }

    void refresh_probe_locked() {
        available_ = false;
        version_.clear();
        models_.clear();
        unavailable_reason_.clear();
        readiness_ = redclaw::protocol::AgentProviderReadinessV1::kProbeFailed;
        std::string path;
        if (process_ == nullptr
            || !process_->executable_available("cursor-agent", &path)) {
            readiness_ = redclaw::protocol::AgentProviderReadinessV1::kCliMissing;
            unavailable_reason_ = "cursor-agent Headless CLI is not installed";
            probe_complete_ = true;
            return;
        }

        int version_exit = 1;
        int status_exit = 1;
        int models_exit = 1;
        std::string version;
        std::string status;
        std::string models_output;
        std::string ignored;
        const bool version_ok = process_->run_probe(
            "cursor-agent", {"--version"}, 5000,
            &version_exit, &version, &ignored) && version_exit == 0;
        version_ = version_ok ? version : std::string{};
        const bool status_ran = process_->run_probe(
            "cursor-agent", {"status"}, 10000,
            &status_exit, &status, &ignored);
        if (!status_ran || status_exit != 0 || !cursor_account_ready(status)) {
            readiness_ = cursor_reports_not_authenticated(status)
                ? redclaw::protocol::AgentProviderReadinessV1::kNotAuthenticated
                : redclaw::protocol::AgentProviderReadinessV1::kProbeFailed;
            unavailable_reason_ = readiness_
                    == redclaw::protocol::AgentProviderReadinessV1::kNotAuthenticated
                ? "cursor-agent account is not authenticated"
                : "cursor-agent account readiness probe failed";
            probe_complete_ = true;
            return;
        }
        bool models_ran = process_->run_probe(
            "cursor-agent", {"models"}, 30000,
            &models_exit, &models_output, &ignored);
        if (!models_ran || models_exit != 0) {
            models_output.clear();
            ignored.clear();
            models_ran = process_->run_probe(
                "cursor-agent", {"models"}, 30000,
                &models_exit, &models_output, &ignored);
        }
        const bool models_ok = models_ran && models_exit == 0;
        if (!version_ok || !models_ok) {
            unavailable_reason_ = "cursor-agent model readiness probe failed";
            probe_complete_ = true;
            return;
        }
        models_ = parse_grok_models(models_output);
        if (models_.empty()) {
            readiness_ = redclaw::protocol::AgentProviderReadinessV1::kModelUnavailable;
            unavailable_reason_ = "no Grok model is available for this cursor-agent account";
            probe_complete_ = true;
            return;
        }
        available_ = true;
        readiness_ = redclaw::protocol::AgentProviderReadinessV1::kReady;
        probe_complete_ = true;
    }

    void on_stdout(std::string line) {
        boost::system::error_code json_error;
        const auto parsed = boost::json::parse(line, json_error);
        if (json_error || !parsed.is_object()) {
            queue_event({
                .task_id = active_task_copy(),
                .event_kind = "provider_protocol_error",
                .text = "cursor-agent emitted invalid stream-json",
                .error_code = "invalid_json",
                .state = redclaw::protocol::AgentTaskStateV1::kFailed,
                .terminal = true,
            });
            return;
        }
        const auto& object = parsed.as_object();
        const std::string task_id = active_task_copy();
        const std::string chat_id = string_field(object, "chat_id")
            .value_or(string_field(object, "session_id").value_or(std::string{}));
        if (!chat_id.empty()) {
            std::lock_guard lock(mutex_);
            chat_ids_[task_id] = chat_id;
        }
        std::string text = string_field(object, "text")
            .value_or(string_field(object, "delta").value_or(std::string{}));
        for (std::size_t offset = 0; offset < text.size();
             offset += redclaw::protocol::kMaxAgentEventChunkBytes) {
            queue_event({
                .task_id = task_id,
                .event_kind = string_field(object, "type").value_or("cursor_event"),
                .text = text.substr(offset, redclaw::protocol::kMaxAgentEventChunkBytes),
                .state = redclaw::protocol::AgentTaskStateV1::kRunning,
                .text_delta = true,
            });
        }
    }

    void on_stderr(std::string line) {
        if (line.size() > redclaw::protocol::kMaxAgentEventChunkBytes) {
            line.resize(redclaw::protocol::kMaxAgentEventChunkBytes);
        }
        queue_event({
            .task_id = active_task_copy(),
            .event_kind = "provider_stderr",
            .text = std::move(line),
            .state = redclaw::protocol::AgentTaskStateV1::kRunning,
            .text_delta = true,
        });
    }

    void on_exit(int exit_code) {
        std::string task_id;
        bool shutting_down = false;
        {
            std::lock_guard lock(mutex_);
            task_id = std::exchange(active_task_id_, {});
            shutting_down = shutting_down_;
        }
        if (task_id.empty() || shutting_down) {
            return;
        }
        queue_event({
            .task_id = task_id,
            .event_kind = exit_code == 0 ? "turn_completed" : "provider_exit",
            .text = exit_code == 0 ? "Cursor Agent turn completed"
                                   : "Cursor Agent exited with code " + std::to_string(exit_code),
            .error_code = exit_code == 0 ? std::string{} : "provider_exit",
            .state = exit_code == 0
                ? redclaw::protocol::AgentTaskStateV1::kCompleted
                : redclaw::protocol::AgentTaskStateV1::kFailed,
            .terminal = true,
        });
    }

    std::string active_task_copy() {
        std::lock_guard lock(mutex_);
        return active_task_id_;
    }

    void queue_event(AgentProviderEvent event) {
        {
            std::lock_guard lock(mutex_);
            emit_locked(std::move(event));
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
    std::optional<PendingTurn> pending_;
    std::unordered_map<std::string, std::string> chat_ids_;
    std::unordered_map<std::string, std::string> task_models_;
    std::unordered_map<std::string, std::filesystem::path> task_directories_;
    std::string active_task_id_;
    std::string version_;
    std::vector<std::string> models_;
    std::string unavailable_reason_;
    std::uint64_t next_approval_id_ = 1;
    bool probe_complete_ = false;
    bool available_ = false;
    redclaw::protocol::AgentProviderReadinessV1 readiness_ =
        redclaw::protocol::AgentProviderReadinessV1::kProbeFailed;
    bool shutting_down_ = false;
    std::thread dispatch_thread_;
};

}  // namespace

std::unique_ptr<IAgentProvider> make_cursor_agent_provider(
    std::unique_ptr<IAgentProcess> process) {
    return std::make_unique<CursorAgentProvider>(std::move(process));
}

}  // namespace redclaw::agent
