#include "redclaw/agent/agent_peer_session.h"

#include <algorithm>
#include <chrono>

namespace redclaw::agent {
namespace {
constexpr std::size_t kRequestCapacity = 64;
constexpr std::size_t kResultCapacity = 256;
constexpr std::size_t kSyncCapacity = 128;
bool fail(std::string* error, const char* reason) {
    if (error) *error = reason;
    return false;
}
}  // namespace

AgentMessageRoute agent_message_route(redclaw::protocol::AgentMessageTypeV1 type) {
    using Type = redclaw::protocol::AgentMessageTypeV1;
    switch (type) {
    case Type::kTaskCreate: case Type::kTurnStart: case Type::kTurnSteer:
    case Type::kTurnInterrupt: case Type::kTaskSyncRequest: case Type::kEventAck:
    case Type::kApprovalDecision:
        return AgentMessageRoute::kLocalExecutor;
    case Type::kCapabilities: case Type::kProjectCatalog: case Type::kTaskSnapshot:
    case Type::kEvent: case Type::kApprovalRequest: case Type::kTaskComplete:
    case Type::kTaskError:
        return AgentMessageRoute::kRemoteTaskClient;
    }
    return AgentMessageRoute::kInvalid;
}

AgentPeerSession::AgentPeerSession(std::unique_ptr<AgentExecutor> executor, Deliver client,
    std::function<bool()> mutations_allowed)
    : executor_(std::move(executor)), client_(std::move(client)), mutations_allowed_(std::move(mutations_allowed)) {}
AgentPeerSession::~AgentPeerSession() { request_stop(); }

void AgentPeerSession::queue_client_sync_locked(const std::string& task_id) {
    if (sync_tasks_.contains("")) return;
    if (task_id.empty() || sync_tasks_.size() >= kSyncCapacity) {
        sync_tasks_.clear();
        sync_tasks_.insert("");
    } else {
        sync_tasks_.insert(task_id);
    }
}

void AgentPeerSession::disconnect_locked() {
    ++state_.generation;
    state_.channel_open = false;
    state_.remote_authorized = false;
    for (const auto& message : requests_) queue_client_sync_locked(message.task_id);
    requests_.clear();
    results_.clear();
    executor_->disconnect();
}

void AgentPeerSession::open(std::string local_epoch) {
    std::lock_guard lock(mutex_);
    if (stopped_ || local_epoch.empty()) return;
    if (state_.channel_open && local_epoch_ == local_epoch) return;
    // Agent-only recreation keeps the epoch and replay watermark. A real
    // desktop-session reset explicitly invalidates them via reset().
    if (local_epoch_ != local_epoch) {
        incoming_.reset();
        state_.last_wire_message_id = 0;
    }
    local_epoch_ = std::move(local_epoch);
    ++state_.generation;
    state_.channel_open = true;
    state_.remote_authorized = false;
    requests_.clear();
    results_.clear();
    prefer_request_ = true;
    executor_->connect(local_epoch_);
    Message sync;
    sync.type = redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest;
    requests_.push_back(std::move(sync));
    state_.request_peak = std::max(state_.request_peak, requests_.size());
}

void AgentPeerSession::close() {
    std::lock_guard lock(mutex_);
    disconnect_locked();
}
void AgentPeerSession::reset() {
    std::lock_guard lock(mutex_);
    disconnect_locked();
    incoming_.reset();
    local_epoch_.clear();
    sync_tasks_.clear();
    state_.last_wire_message_id = 0;
}
void AgentPeerSession::request_stop() {
    std::lock_guard lock(mutex_);
    if (stopped_) return;
    stopped_ = true;
    disconnect_locked();
    executor_->request_stop();
}

bool AgentPeerSession::receive(const Message& message, std::uint64_t generation,
                               std::string* error) {
    const auto route = agent_message_route(message.type);
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || !state_.channel_open || state_.generation != generation) {
            ++state_.stale_total;
            return fail(error, "stale Agent channel generation");
        }
        if (route == AgentMessageRoute::kInvalid) {
            ++state_.rejected_total;
            return fail(error, "invalid Agent message type");
        }
        if (!incoming_.accept(message, error)) {
            ++state_.rejected_total;
            return false;
        }
        if (route == AgentMessageRoute::kLocalExecutor) {
            ++state_.received_commands;
            // Enqueues only; never invokes a provider on the network thread.
            return executor_->handle_message(message, error);
        }
        ++state_.received_results;
        if (const auto authorized = redclaw::protocol::agent_capabilities_authorization_update_v1(message))
            state_.remote_authorized = *authorized;
        using Type = redclaw::protocol::AgentMessageTypeV1;
        if (message.type == Type::kEvent || message.type == Type::kApprovalRequest
            || message.type == Type::kTaskComplete || message.type == Type::kTaskError)
            ++state_.received_task_events;
    }
    if (client_(message, error)) return true;
    std::lock_guard lock(mutex_);
    if (generation == state_.generation) {
        queue_client_sync_locked(message.task_id);
    }
    return false;
}

bool AgentPeerSession::enqueue_request(Message message, std::string* error) {
    if (agent_message_route(message.type) != AgentMessageRoute::kLocalExecutor)
        return fail(error, "local API accepts Agent requests only");
    if (!redclaw::protocol::validate_agent_message_v1(message, error)) return false;
    std::lock_guard lock(mutex_);
    if (agent_request_mutates_workspace(message.type) && mutations_allowed_ && !mutations_allowed_())
        return fail(error, "workspace_transfer_busy");
    if (stopped_ || !state_.channel_open || requests_.size() >= kRequestCapacity) {
        if (!stopped_) queue_client_sync_locked(message.task_id);
        return fail(error, "Agent request queue unavailable or full");
    }
    requests_.push_back(std::move(message));
    state_.request_peak = std::max(state_.request_peak, requests_.size());
    return true;
}

void AgentPeerSession::retry_client_sync() {
    Message notice;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (!state_.channel_open || sync_tasks_.empty()) return;
        generation = state_.generation;
        notice.task_id = *sync_tasks_.begin();
    }
    notice.type = redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest;
    notice.session_epoch = "runtime-local-delivery";
    notice.message_id = 1;
    if (client_(notice, nullptr)) {
        std::lock_guard lock(mutex_);
        if (generation == state_.generation) sync_tasks_.erase(notice.task_id);
    }
}

void AgentPeerSession::pump(const Deliver& send) {
    const auto started = std::chrono::steady_clock::now();
    retry_client_sync();
    std::size_t capacity = 0;
    std::uint64_t collected_generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (stopped_ || !state_.channel_open) return;
        capacity = kResultCapacity - results_.size();
        collected_generation = state_.generation;
    }
    auto outgoing = executor_->take_outbound(std::min<std::size_t>(8, capacity));
    {
        std::lock_guard lock(mutex_);
        for (auto& message : outgoing) {
            if (state_.channel_open && state_.generation == collected_generation
                && message.session_epoch == local_epoch_
                && results_.size() < kResultCapacity) results_.push_back(std::move(message));
            else ++state_.stale_total;
        }
        state_.result_peak = std::max(state_.result_peak, results_.size());
    }
    // One budget for BOTH directions, round-robin so a result flood cannot
    // starve local approvals/interrupts and requests cannot starve results.
    for (std::size_t sent = 0; sent < 8
         && std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1); ++sent) {
        Message message;
        bool request = false;
        std::size_t request_offset = 0;
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(mutex_);
            if (!state_.channel_open || (requests_.empty() && results_.empty())) break;
            auto next_request = requests_.begin();
            if (mutations_allowed_ && !mutations_allowed_()) {
                next_request = requests_.end();
                for (auto candidate = requests_.begin(); candidate != requests_.end(); ++candidate) {
                    if (agent_request_mutates_workspace(candidate->type)) continue;
                    // Do not ask the peer to synchronize a task whose create
                    // request is still held locally: that would report a false
                    // task_not_found. ACKs and existing-task output can flow.
                    if (candidate->type == redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest
                        && !candidate->task_id.empty() && std::any_of(requests_.begin(), candidate, [&](const auto& earlier) {
                            return earlier.type == redclaw::protocol::AgentMessageTypeV1::kTaskCreate
                                && earlier.task_id == candidate->task_id;
                        })) continue;
                    next_request = candidate; break;
                }
            }
            request = next_request != requests_.end() && (prefer_request_ || results_.empty());
            if (!request && results_.empty()) break;
            if (request) request_offset = static_cast<std::size_t>(std::distance(requests_.begin(), next_request));
            auto& queue = request ? requests_ : results_;
            // Keep the selected request/result until send succeeds.
            message = queue[request ? request_offset : 0];
            message.session_epoch = local_epoch_;
            message.message_id = ++state_.last_wire_message_id;
            message.sent_at_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            generation = state_.generation;
        }
        std::string error;
        const bool accepted = send(message, &error); // Never hold the session lock over callbacks.
        std::lock_guard lock(mutex_);
        if (generation != state_.generation) { ++state_.stale_total; break; }
        if (!accepted) break;
        auto& queue = request ? requests_ : results_;
        queue.erase(queue.begin() + (request ? static_cast<std::ptrdiff_t>(request_offset) : 0));
        prefer_request_ = !request;
        if (request) ++state_.sent_requests; else ++state_.sent_results;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::lock_guard lock(mutex_);
    state_.max_pump_us = std::max(state_.max_pump_us, static_cast<std::uint64_t>(elapsed));
}

std::size_t AgentPeerSession::request_capacity() const {
    std::lock_guard lock(mutex_);
    return state_.channel_open ? kRequestCapacity - requests_.size() : 0;
}
AgentPeerSnapshot AgentPeerSession::snapshot() const {
    std::lock_guard lock(mutex_);
    auto result = state_;
    result.request_depth = requests_.size();
    result.result_depth = results_.size();
    result.client_sync_pending = sync_tasks_.size();
    return result;
}
AgentExecutorSnapshot AgentPeerSession::executor_snapshot() const { return executor_->snapshot(); }

}  // namespace redclaw::agent
