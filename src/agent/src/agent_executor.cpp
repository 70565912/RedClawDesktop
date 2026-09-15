#include "redclaw/agent/agent_executor.h"

#include <algorithm>
#include <chrono>

namespace redclaw::agent {
namespace {
constexpr std::size_t kMaxCommands = 128;
constexpr std::size_t kMaxCommandBytes = 1024U * 1024U;
// Validated envelopes are at most 64 KiB; this also bounds the queue to 4 MiB.
constexpr std::size_t kMaxOutbound = 4U * 1024U * 1024U
    / redclaw::protocol::kMaxAgentMessageBytes;
std::uint64_t elapsed_ms(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - since).count());
}
}  // namespace

AgentExecutor::AgentExecutor(std::unique_ptr<RemoteAgentBroker> broker)
    : broker_(std::move(broker)), worker_([this](std::stop_token stop) { run(stop); }) {}

AgentExecutor::~AgentExecutor() {
    request_stop();
    // The Runtime owner tears this down after unregistering network callbacks,
    // never from a GUI or a libdatachannel callback.
    worker_.join();
}

void AgentExecutor::request_stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        commands_.clear();
        snapshot_.queued_bytes = 0;
    }
    worker_.request_stop();
    changed_.notify_one();
}

void AgentExecutor::connect(std::string epoch) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        desired_epoch_ = std::move(epoch);
        ++connection_revision_;
        commands_.clear();
        outbound_.clear();
        rejections_.clear();
        snapshot_.queued_bytes = 0;
    }
    changed_.notify_one();
}

void AgentExecutor::disconnect() { connect({}); }

bool AgentExecutor::handle_message(
    const redclaw::protocol::AgentMessageEnvelopeV1& message, std::string* error) {
    if (!redclaw::protocol::validate_agent_message_v1(message, error)) return false;
    const auto bytes = redclaw::protocol::agent_message_protobuf_size_v1(message);
    {
        std::lock_guard lock(mutex_);
        if (agent_request_mutates_workspace(message.type) && !broker_->allows_workspace_mutations()) {
            ++snapshot_.rejected_total;
            reject_locked(message, "workspace_transfer_busy");
            if (error) *error = "workspace_transfer_busy";
            return false;
        }
        if (stopping_ || desired_epoch_.empty() || commands_.size() >= kMaxCommands
            || bytes > kMaxCommandBytes - snapshot_.queued_bytes) {
            ++snapshot_.rejected_total;
            reject_locked(message, "agent_busy: executor unavailable or queue capacity exhausted");
            if (error) *error = "agent_busy: executor unavailable or queue capacity exhausted";
            return false;
        }
        commands_.push_back({message, bytes, std::chrono::steady_clock::now()});
        snapshot_.queued_bytes += bytes;
    }
    changed_.notify_one();
    return true;
}

void AgentExecutor::reject_locked(const redclaw::protocol::AgentMessageEnvelopeV1& request,
    const std::string& error) {
    snapshot_.last_error = error;
    if (rejections_.size() >= 32 || desired_epoch_.empty()) return;
    redclaw::protocol::AgentMessageEnvelopeV1 response;
    response.session_epoch = desired_epoch_;
    response.task_id = request.task_id;
    response.request_id = request.request_id;
    response.type = request.task_id.empty()
        ? redclaw::protocol::AgentMessageTypeV1::kCapabilities
        : redclaw::protocol::AgentMessageTypeV1::kTaskError;
    response.task_state = redclaw::protocol::AgentTaskStateV1::kPaused;
    response.event_kind = "request_rejected";
    response.error_code = "agent_request_rejected";
    response.text = error.substr(0, redclaw::protocol::kMaxAgentEventChunkBytes);
    rejections_.push_back(std::move(response));
}

std::vector<redclaw::protocol::AgentMessageEnvelopeV1>
AgentExecutor::take_outbound(std::size_t limit) {
    std::vector<redclaw::protocol::AgentMessageEnvelopeV1> result;
    std::lock_guard lock(mutex_);
    while (!rejections_.empty() && result.size() < limit) {
        result.push_back(std::move(rejections_.front()));
        rejections_.pop_front();
    }
    while (!outbound_.empty() && result.size() < limit) {
        result.push_back(std::move(outbound_.front()));
        outbound_.pop_front();
    }
    changed_.notify_one();
    return result;
}

AgentExecutorSnapshot AgentExecutor::snapshot() const {
    std::lock_guard lock(mutex_);
    auto result = snapshot_;
    result.queued_commands = commands_.size();
    result.outbound_count += outbound_.size() + rejections_.size();
    return result;
}

void AgentExecutor::run(std::stop_token stop) {
    std::uint64_t applied_revision = 0;
    while (!stop.stop_requested()) {
        std::optional<Command> command;
        std::string epoch;
        std::uint64_t revision = 0;
        std::size_t capacity = 0;
        {
            std::unique_lock lock(mutex_);
            changed_.wait_for(lock, std::chrono::milliseconds(10), [&] {
                return stopping_ || connection_revision_ != applied_revision || !commands_.empty();
            });
            if (stopping_) break;
            revision = connection_revision_;
            epoch = desired_epoch_;
            capacity = kMaxOutbound - outbound_.size();
            if (revision == applied_revision && !commands_.empty()) {
                command = std::move(commands_.front());
                commands_.pop_front();
                snapshot_.queued_bytes -= command->bytes;
                snapshot_.max_queue_wait_ms = std::max(
                    snapshot_.max_queue_wait_ms, elapsed_ms(command->enqueued));
            }
        }
        const auto started = std::chrono::steady_clock::now();
        std::string error;
        try {
            if (revision != applied_revision) {
                broker_->disconnect();
                if (!epoch.empty()) broker_->connect(epoch);
                applied_revision = revision;
            }
            if (command) (void)broker_->handle_message(command->message, &error);
            broker_->tick();
            auto messages = broker_->take_outbound(std::min<std::size_t>(8, capacity));
            const auto metrics = broker_->metrics();
            const auto execution = broker_->execution_snapshot();
            const auto cached_bytes = broker_->cached_event_bytes();
            const auto broker_outbound = broker_->queued_outbound_count();
            std::lock_guard lock(mutex_);
            if (revision == connection_revision_ && !stopping_) {
                for (auto& message : messages) outbound_.push_back(std::move(message));
            }
            snapshot_.broker = metrics;
            snapshot_.execution = execution;
            snapshot_.cached_event_bytes = cached_bytes;
            snapshot_.outbound_count = broker_outbound;
            snapshot_.max_processing_ms = std::max(snapshot_.max_processing_ms, elapsed_ms(started));
            if (command) ++snapshot_.processed_total;
            if (!error.empty() && command && revision == connection_revision_) {
                reject_locked(command->message, error);
            }
        } catch (const std::exception&) {
            std::lock_guard lock(mutex_);
            snapshot_.last_error = "Agent worker failed; desktop remains independent";
            stopping_ = true;
        }
    }
    broker_->shutdown();
}
}  // namespace redclaw::agent
