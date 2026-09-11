#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include "redclaw/agent/agent_peer_session.h"

namespace {
using namespace redclaw::agent;
using namespace redclaw::protocol;
using Message = AgentMessageEnvelopeV1;
using Type = AgentMessageTypeV1;

struct Counts {
    std::atomic<unsigned> starts{0}, turns{0}, interrupts{0}, approvals{0};
};

class Workspace final : public IAgentWorkspaceManager {
    bool prepare(const AgentProjectRegistration& project, const std::string& task,
        AgentWorkDirectoryModeV1, std::filesystem::path* path, std::string*) override {
        *path = project.root / task;
        return true;
    }
};

class Provider final : public IAgentProvider {
public:
    explicit Provider(std::shared_ptr<Counts> counts) : counts_(std::move(counts)) {}
    AgentProviderProbe probe(bool) override {
        return {.provider = AgentProviderKindV1::kCodex, .available = true,
            .readiness = AgentProviderReadinessV1::kReady, .version = "fake-peer",
            .supports_structured_approval = true};
    }
    void set_event_sink(AgentProviderEventSink sink) override { sink_ = std::move(sink); }
    bool start_task(const AgentProviderTaskRequest& task, std::string*) override {
        ++counts_->starts;
        sink_({.task_id = task.task_id, .event_kind = "started", .text = "fixture"});
        if (task.instruction == "bulk") {
            for (unsigned i = 0; i < 128; ++i)
                sink_({.task_id = task.task_id, .event_kind = "text_delta",
                    .text = std::string(8192, 'x'), .text_delta = true});
        }
        return true;
    }
    bool resume_task(const AgentProviderTaskRequest& task, std::string* error) override {
        return start_task(task, error);
    }
    bool start_turn(const std::string& task, const std::string&, std::string*) override {
        ++counts_->turns;
        sink_({.task_id = task, .event_kind = "completed", .text = "done",
            .state = AgentTaskStateV1::kCompleted, .terminal = true});
        return true;
    }
    bool steer(const std::string& task, const std::string&, std::string*) override {
        sink_({.task_id = task, .request_id = "same-approval", .event_kind = "command_approval",
            .text = "approve fixture", .state = AgentTaskStateV1::kAwaitingApproval,
            .approval_request = true});
        return true;
    }
    bool interrupt(const std::string&, std::string*) override { ++counts_->interrupts; return true; }
    bool respond_to_approval(const std::string&, const std::string&,
        AgentApprovalDecisionV1, std::string*) override { ++counts_->approvals; return true; }
    void shutdown() override {}
private:
    std::shared_ptr<Counts> counts_;
    AgentProviderEventSink sink_;
};

struct Endpoint {
    std::shared_ptr<Counts> counts = std::make_shared<Counts>();
    std::vector<Message> client, wire;
    std::unique_ptr<AgentPeerSession> peer;
    explicit Endpoint(bool authorized) {
        auto broker = std::make_unique<RemoteAgentBroker>(
            RemoteAgentBrokerConfig{.authorized = authorized}, std::make_unique<Workspace>());
        broker->add_project({.project_id = "project", .display_name = "Fixture",
            .root = std::filesystem::temp_directory_path() / "peer-fixture", .git_repository = true});
        broker->add_provider(std::make_unique<Provider>(counts));
        peer = std::make_unique<AgentPeerSession>(
            std::make_unique<AgentExecutor>(std::move(broker)),
            [this](const Message& message, std::string*) { client.push_back(message); return true; });
    }
    void pump_to(Endpoint& other) {
        std::size_t calls = 0;
        peer->pump([&](const Message& message, std::string*) {
            ++calls;
            const auto serialized = serialize_agent_message_v1(message);
            const auto parsed = parse_agent_message_v1(serialized);
            EXPECT_TRUE(parsed.ok);
            EXPECT_LE(serialized.size(), kMaxAgentMessageBytes);
            if (!wire.empty()) EXPECT_GT(message.message_id, wire.back().message_id);
            wire.push_back(message);
            std::string error;
            EXPECT_TRUE(other.peer->receive(parsed.value, other.peer->snapshot().generation, &error)) << error;
            return true;
        });
        EXPECT_LE(calls, 8U);
        EXPECT_LE(peer->snapshot().request_depth, 64U);
        EXPECT_LE(peer->snapshot().result_depth, 256U);
    }
};

Message request(Type type = Type::kTaskCreate, std::string task = "same-task") {
    Message message;
    message.type = type;
    message.session_epoch = "local-api";
    message.message_id = 1;
    message.task_id = std::move(task);
    message.request_id = "same-request";
    message.provider = AgentProviderKindV1::kCodex;
    message.project_id = "project";
    message.text = "fixture";
    return message;
}

template <typename Predicate>
bool drain(Endpoint& a, Endpoint& b, Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        a.pump_to(b);
        b.pump_to(a);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

TEST(AgentPeerSession, ClassifiesEveryMessageWithoutDesktopRole) {
    for (const auto type : {Type::kTaskCreate, Type::kTurnStart, Type::kTurnSteer,
        Type::kTurnInterrupt, Type::kTaskSyncRequest, Type::kEventAck, Type::kApprovalDecision})
        EXPECT_EQ(agent_message_route(type), AgentMessageRoute::kLocalExecutor);
    for (const auto type : {Type::kCapabilities, Type::kProjectCatalog, Type::kTaskSnapshot,
        Type::kEvent, Type::kApprovalRequest, Type::kTaskComplete, Type::kTaskError})
        EXPECT_EQ(agent_message_route(type), AgentMessageRoute::kRemoteTaskClient);
    EXPECT_EQ(agent_message_route(static_cast<Type>(255)), AgentMessageRoute::kInvalid);
}

TEST(AgentPeerSession, AuthorizationIsIndependentFromRequestDirection) {
    for (const bool allow_a : {false, true}) for (const bool allow_b : {false, true}) {
        Endpoint a(allow_a), b(allow_b);
        a.peer->open("epoch-a"); b.peer->open("epoch-b");
        ASSERT_TRUE(a.peer->enqueue_request(request()));
        ASSERT_TRUE(b.peer->enqueue_request(request()));
        ASSERT_TRUE(drain(a, b, [&] {
            return a.peer->executor_snapshot().processed_total >= 2
                && b.peer->executor_snapshot().processed_total >= 2
                && a.peer->snapshot().received_results >= 3 && b.peer->snapshot().received_results >= 3;
        }));
        EXPECT_EQ(a.counts->starts, allow_a ? 1U : 0U);
        EXPECT_EQ(b.counts->starts, allow_b ? 1U : 0U);
        EXPECT_EQ(a.peer->snapshot().remote_authorized, allow_b);
        EXPECT_EQ(b.peer->snapshot().remote_authorized, allow_a);
        for (const auto& message : a.client)
            EXPECT_EQ(agent_message_route(message.type), AgentMessageRoute::kRemoteTaskClient);
    }
}

TEST(AgentPeerSession, SameTaskAndApprovalIdsStayOnTheirExecutionSide) {
    Endpoint a(true), b(true);
    a.peer->open("epoch-a"); b.peer->open("epoch-b");
    ASSERT_TRUE(a.peer->enqueue_request(request()));
    ASSERT_TRUE(b.peer->enqueue_request(request()));
    ASSERT_TRUE(drain(a,b,[&] { return a.counts->starts == 1 && b.counts->starts == 1; }));
    ASSERT_TRUE(a.peer->enqueue_request(request(Type::kTurnSteer)));
    ASSERT_TRUE(b.peer->enqueue_request(request(Type::kTurnSteer)));
    ASSERT_TRUE(drain(a,b,[&] {
        auto has = [](const Endpoint& e) { return std::any_of(e.client.begin(),e.client.end(),
            [](const Message& m) { return m.type == Type::kApprovalRequest; }); };
        return has(a) && has(b);
    }));
    auto decision = request(Type::kApprovalDecision);
    decision.request_id = "same-approval";
    decision.approval_decision = AgentApprovalDecisionV1::kAccept;
    ASSERT_TRUE(a.peer->enqueue_request(decision));
    ASSERT_TRUE(drain(a,b,[&] { return b.counts->approvals == 1; }));
    EXPECT_EQ(a.counts->approvals, 0U);
    a.peer->close(); b.peer->close();
    a.peer->open("epoch-a"); b.peer->open("epoch-b");
    ASSERT_TRUE(drain(a,b,[&] {
        return a.peer->snapshot().remote_authorized && b.peer->snapshot().remote_authorized;
    }));
    EXPECT_EQ(a.counts->starts, 1U);
    EXPECT_EQ(b.counts->starts, 1U);
    EXPECT_EQ(a.counts->approvals, 1U); // Disconnect denies its pending approval.
}

TEST(AgentPeerSession, ReplayEpochAndOldGenerationAreRejected) {
    Endpoint e(true); e.peer->open("local");
    auto message = request(Type::kCapabilities, "");
    message.session_epoch = "remote"; message.text.clear();
    const auto generation = e.peer->snapshot().generation;
    ASSERT_TRUE(e.peer->receive(message, generation));
    EXPECT_FALSE(e.peer->receive(message, generation));
    message.message_id = 2; message.session_epoch = "old";
    EXPECT_FALSE(e.peer->receive(message, generation));
    e.peer->close(); e.peer->open("local");
    message.session_epoch = "remote";
    EXPECT_FALSE(e.peer->receive(message, generation));
    EXPECT_TRUE(e.peer->receive(message, e.peer->snapshot().generation));
    EXPECT_FALSE(e.peer->enqueue_request(message)); // Results cannot become delegated tasks.
}

TEST(AgentPeerSession, BackpressureIsBoundedAndReentrantCloseDoesNotPopNewQueue) {
    Endpoint e(true); e.peer->open("local");
    for (unsigned i=0; i<63; ++i) ASSERT_TRUE(e.peer->enqueue_request(request()));
    EXPECT_FALSE(e.peer->enqueue_request(request()));
    e.peer->pump([](const Message&, std::string*) { return false; });
    EXPECT_EQ(e.peer->snapshot().request_depth,64U);
    e.peer->pump([&](const Message&, std::string*) {
        e.peer->close(); e.peer->open("local"); return true;
    });
    EXPECT_EQ(e.peer->snapshot().request_depth,1U);
    EXPECT_GT(e.peer->snapshot().stale_total,0U);
}

TEST(AgentPeerSession, BidirectionalMegabyteResultsUseOneBoundedBudget) {
    Endpoint a(true), b(true);
    a.peer->open("epoch-a"); b.peer->open("epoch-b");
    auto bulk=request(); bulk.text="bulk";
    ASSERT_TRUE(a.peer->enqueue_request(bulk));
    ASSERT_TRUE(b.peer->enqueue_request(bulk));
    auto bytes=[](const Endpoint& e) {
        std::size_t n=0; for (const auto& m:e.client)
            if (m.event_kind=="text_delta") n+=m.text.size();
        return n;
    };
    ASSERT_TRUE(drain(a,b,[&] { return bytes(a)>=1024U*1024U && bytes(b)>=1024U*1024U; }));
    EXPECT_EQ(a.counts->starts,1U);
    EXPECT_EQ(b.counts->starts,1U);
}

TEST(AgentPeerSession, DisconnectedLocalRequestsRequireSyncAndAreNeverResent) {
    Endpoint a(true), b(true);
    a.peer->open("epoch-a"); b.peer->open("epoch-b");
    ASSERT_TRUE(a.peer->enqueue_request(request()));
    a.peer->close();
    EXPECT_FALSE(a.peer->enqueue_request(request(Type::kTurnStart)));
    a.peer->open("epoch-a");
    ASSERT_TRUE(drain(a, b, [&] {
        return a.peer->snapshot().remote_authorized
            && a.peer->snapshot().client_sync_pending == 0;
    }));
    EXPECT_EQ(b.counts->starts, 0U);
    EXPECT_TRUE(std::any_of(a.client.begin(), a.client.end(), [](const Message& m) {
        return m.type == Type::kTaskSyncRequest;
    }));
}
} // namespace
