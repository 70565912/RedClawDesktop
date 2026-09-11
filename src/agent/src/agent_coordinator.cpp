#include "redclaw/agent/agent_coordinator.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace redclaw::agent {
namespace {
constexpr std::size_t kCapacity = 128;
constexpr std::size_t kByteCapacity = 4U * 1024U * 1024U;
std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
}  // namespace

class AgentCoordinator::Impl {
public:
    explicit Impl(NormalAgentControlConfigV1 config) : state(std::move(config)) {}
    struct Work {
        redclaw::protocol::AgentMessageEnvelopeV1 message;
        bool remote;
        std::uint64_t cookie;
        std::uint64_t enqueued;
        std::size_t bytes;
    };
    NormalAgentControlStateV1 state;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Work> work;
    std::deque<AgentCoordinationResult> results;
    std::unordered_set<std::string> requested_sync;
    std::size_t bytes = 0;
    AgentCoordinatorSnapshot status;
    bool stopping = false;
    bool finished = false;
    std::thread worker;

    void publish(AgentCoordinationResult result) {
        std::unique_lock lock(mutex);
        if (result.kind == AgentCoordinationResultKind::kDispatch
            && (result.message.type == redclaw::protocol::AgentMessageTypeV1::kEventAck
                || result.message.type == redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest)) {
            const auto found = std::find_if(results.begin(), results.end(), [&](const auto& queued) {
                return queued.kind == result.kind && queued.message.type == result.message.type
                    && queued.message.task_id == result.message.task_id;
            });
            if (found != results.end()) {
                found->message.acknowledged_event_sequence = std::max(
                    found->message.acknowledged_event_sequence, result.message.acknowledged_event_sequence);
                return;
            }
        }
        // A validated envelope is at most 64 KiB. Bound results to 4 MiB too.
        changed.wait(lock, [&] { return stopping || results.size()
            < kByteCapacity / redclaw::protocol::kMaxAgentMessageBytes; });
        if (!stopping) results.push_back(std::move(result));
    }

    void run() {
        std::string error;
        bool ready = state.initialize(&error);
        if (ready && state.authority() == CoordinationAuthorityV1::kNone) {
            ready = state.transfer_authority(CoordinationAuthorityV1::kNone,
                CoordinationAuthorityV1::kNormalAgent, true, true, false,
                "normal_client_enabled", now_ms(), &error);
        }
        const auto epoch = "coordinator-" + std::to_string(now_ms());
        std::uint64_t next_id = 1;
        {
            std::lock_guard lock(mutex);
            status.ready = ready;
            status.error = error;
            status.authority = state.authority();
            status.sync_required = state.sync_required();
            status.parsed_records = state.journal().parsed_record_count();
        }
        while (ready) {
            Work item;
            {
                std::unique_lock lock(mutex);
                changed.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping || !work.empty() || !requested_sync.empty(); });
                if (stopping) break;
                for (const auto& task : requested_sync) state.request_sync(task);
                requested_sync.clear();
                status.sync_required = state.sync_required();
                if (work.empty()) {
                    lock.unlock();
                    for (auto& sync : state.take_sync_requests(epoch, &next_id, now_ms())) {
                        publish({AgentCoordinationResultKind::kDispatch, std::move(sync), 0, now_ms(), now_ms(), {}});
                    }
                    continue;
                }
                item = std::move(work.front());
                work.pop_front();
                bytes -= item.bytes;
            }
            error.clear();
            // Polling is not a replay request. Sending coordination_status as a
            // zero-ACK sync used to retransmit the entire task on every poll,
            // filling both peers' bounded queues with redundant history.
            if (!item.remote && item.message.type
                    == redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest
                && item.message.event_kind == "coordination_status") {
                const bool refreshed = state.refresh(&error);
                const auto task = refreshed ? state.task_snapshot(item.message.task_id)
                                            : std::nullopt;
                if (task) {
                    item.message.task_state = task->task_state;
                    item.message.acknowledged_event_sequence =
                        state.acknowledged_event_sequence(item.message.task_id);
                    item.message.event_sequence = item.message.acknowledged_event_sequence;
                    item.message.text.clear();
                } else if (error.empty()) error = "local task snapshot unavailable; request explicit sync";
                publish({task ? AgentCoordinationResultKind::kLocalSnapshot
                              : AgentCoordinationResultKind::kRejected,
                    std::move(item.message), item.cookie, item.enqueued, now_ms(), error});
                continue;
            }
            bool apply_to_view = true;
            const auto previous_ack = state.acknowledged_event_sequence(item.message.task_id);
            const bool accepted = item.remote
                ? state.observe_remote(item.message, now_ms(), &error, &apply_to_view)
                : state.prepare_outbound(item.message, item.message.supersedes_request_id,
                    item.message.type == redclaw::protocol::AgentMessageTypeV1::kApprovalDecision
                        ? "explicit_decision" : "scoped_request", now_ms(), &error);
            const auto persisted = now_ms();
            if (accepted && (!item.remote || (apply_to_view
                && item.message.error_code == "task_not_found"
                && item.message.event_kind == "sync_unavailable"))) {
                const auto task = state.task_snapshot(item.message.task_id);
                if (task) item.message.task_state = task->task_state;
            }
            if (!accepted || !item.remote || apply_to_view) publish({accepted ? (item.remote ? AgentCoordinationResultKind::kRemoteDurable
                                             : AgentCoordinationResultKind::kDispatch)
                              : AgentCoordinationResultKind::kRejected,
                item.message, item.cookie, item.enqueued, persisted, error});
            if (accepted && item.remote) {
                if (state.acknowledged_event_sequence(item.message.task_id) > previous_ack) {
                    redclaw::protocol::AgentMessageEnvelopeV1 ack;
                    ack.session_epoch = epoch;
                    ack.message_id = next_id++;
                    ack.sent_at_ms = now_ms();
                    ack.type = redclaw::protocol::AgentMessageTypeV1::kEventAck;
                    ack.task_id = item.message.task_id;
                    ack.acknowledged_event_sequence = state.acknowledged_event_sequence(ack.task_id);
                    publish({AgentCoordinationResultKind::kDispatch, std::move(ack), 0,
                        item.enqueued, persisted, {}});
                }
                for (auto& sync : state.take_sync_requests(epoch, &next_id, now_ms())) {
                    publish({AgentCoordinationResultKind::kDispatch, std::move(sync), 0,
                        item.enqueued, persisted, {}});
                }
            }
            {
                std::lock_guard lock(mutex);
                status.authority = state.authority();
                status.sync_required = state.sync_required();
                status.parsed_records = state.journal().parsed_record_count();
                if (!accepted) { ++status.rejected_total; status.error = error; }
                if (state.journal().invalid()) ready = false;
            }
        }
        {
            std::lock_guard lock(mutex);
            status.ready = false;
            if (!stopping && status.error.empty()) status.error = "Agent coordination unavailable";
        }
        for (;;) {
            std::optional<Work> rejected;
            {
                std::lock_guard lock(mutex);
                if (stopping || work.empty()) break;
                rejected = std::move(work.front()); work.pop_front(); bytes -= rejected->bytes;
            }
            publish({AgentCoordinationResultKind::kRejected, std::move(rejected->message),
                rejected->cookie, rejected->enqueued, now_ms(), "Agent coordination unavailable"});
        }
        std::lock_guard lock(mutex);
        status.ready = false;
        finished = true;
        changed.notify_all();
    }
};

AgentCoordinator::AgentCoordinator(NormalAgentControlConfigV1 config)
    : impl_(std::make_shared<Impl>(std::move(config))) {
    impl_->worker = std::thread([state = impl_] {
        try { state->run(); }
        catch (const std::exception&) {
            std::lock_guard lock(state->mutex);
            state->status.ready = false;
            state->status.error = "Agent coordination worker failed; desktop is unaffected";
            state->finished = true;
            state->changed.notify_all();
        }
    });
}

AgentCoordinator::~AgentCoordinator() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
        impl_->work.clear();
        impl_->changed.notify_all();
    }
    // Disk I/O already submitted must finish atomically. No UI callback or raw
    // owner pointer is captured by this worker; it retains only its own state.
    // Do not wait on filesystem latency in a GUI destructor.
    if (impl_->worker.joinable()) {
        bool finished = false;
        { std::lock_guard lock(impl_->mutex); finished = impl_->finished; }
        if (finished) impl_->worker.join();
        else impl_->worker.detach();
    }
}

bool AgentCoordinator::enqueue(redclaw::protocol::AgentMessageEnvelopeV1 message,
    bool remote, std::uint64_t cookie, std::string* error) {
    if (!redclaw::protocol::validate_agent_message_v1(message, error)) return false;
    const auto bytes = redclaw::protocol::agent_message_protobuf_size_v1(message);
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->finished || (!impl_->status.ready && !impl_->status.error.empty())
        || impl_->work.size() >= kCapacity
        || impl_->bytes + bytes > kByteCapacity) {
        ++impl_->status.rejected_total;
        if (error) *error = "agent_busy: coordination unavailable or queue capacity exhausted";
        return false;
    }
    impl_->bytes += bytes;
    impl_->work.push_back({std::move(message), remote, cookie, now_ms(), bytes});
    impl_->changed.notify_one();
    return true;
}

std::vector<AgentCoordinationResult> AgentCoordinator::take_results(std::size_t limit) {
    std::vector<AgentCoordinationResult> result;
    std::lock_guard lock(impl_->mutex);
    while (!impl_->results.empty() && result.size() < limit) {
        result.push_back(std::move(impl_->results.front()));
        impl_->results.pop_front();
    }
    impl_->changed.notify_one();
    return result;
}

AgentCoordinatorSnapshot AgentCoordinator::snapshot() const {
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->status;
    result.queue_depth = impl_->work.size();
    return result;
}

void AgentCoordinator::request_sync(std::string task_id) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->requested_sync.size() < kCapacity) impl_->requested_sync.insert(std::move(task_id));
    impl_->changed.notify_one();
}
}  // namespace redclaw::agent
