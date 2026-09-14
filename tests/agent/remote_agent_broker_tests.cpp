#include <gtest/gtest.h>
#include <fstream>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "redclaw/agent/remote_agent_broker.h"
#include "redclaw/agent/coordination.h"

namespace {

class FakeWorkspaceManager final : public redclaw::agent::IAgentWorkspaceManager {
public:
    bool prepare(
        const redclaw::agent::AgentProjectRegistration& project,
        const std::string& task_id,
        redclaw::protocol::AgentWorkDirectoryModeV1,
        std::filesystem::path* working_directory,
        std::string*) override {
        *working_directory = project.root / task_id;
        return true;
    }
};

class FakeAgentProvider final : public redclaw::agent::IAgentProvider {
public:
    redclaw::agent::AgentProviderProbe probe(bool) override {
        return {
            .provider = redclaw::protocol::AgentProviderKindV1::kCodex,
            .available = available,
            .readiness = available
                ? redclaw::protocol::AgentProviderReadinessV1::kReady
                : redclaw::protocol::AgentProviderReadinessV1::kProbeFailed,
            .version = "fake-1",
            .models = {},
            .supports_structured_approval = true,
        };
    }


    void set_event_sink(redclaw::agent::AgentProviderEventSink value) override {
        sink = std::move(value);
    }

    bool start_task(
        const redclaw::agent::AgentProviderTaskRequest& request,
        std::string*) override {
        ++start_count;
        started_task = request.task_id;
        return start_succeeds;
    }

    bool resume_task(
        const redclaw::agent::AgentProviderTaskRequest& request,
        std::string* error) override {
        return start_task(request, error);
    }

    bool start_turn(const std::string&, const std::string&, std::string*) override {
        ++turn_count;
        return true;
    }

    bool steer(const std::string&, const std::string&, std::string*) override {
        ++steer_count;
        return true;
    }

    bool interrupt(const std::string&, std::string*) override {
        ++interrupt_count;
        return true;
    }

    bool respond_to_approval(
        const std::string&,
        const std::string&,
        redclaw::protocol::AgentApprovalDecisionV1 decision,
        std::string*) override {
        last_decision = decision;
        return true;
    }

    void shutdown() override {
        ++shutdown_count;
    }

    void emit(redclaw::agent::AgentProviderEvent event) {
        sink(std::move(event));
    }

    bool available = true;
    bool start_succeeds = true;
    int start_count = 0;
    int turn_count = 0;
    int steer_count = 0;
    int interrupt_count = 0;
    int shutdown_count = 0;
    std::string started_task;
    redclaw::protocol::AgentApprovalDecisionV1 last_decision =
        redclaw::protocol::AgentApprovalDecisionV1::kNone;
    redclaw::agent::AgentProviderEventSink sink;
};

redclaw::protocol::AgentMessageEnvelopeV1 task_request(
    std::uint64_t message_id,
    std::string task_id) {
    redclaw::protocol::AgentMessageEnvelopeV1 message;
    message.session_epoch = "peer-epoch";
    message.message_id = message_id;
    message.type = redclaw::protocol::AgentMessageTypeV1::kTaskCreate;
    message.task_id = std::move(task_id);
    message.request_id = "request-" + std::to_string(message_id);
    message.provider = redclaw::protocol::AgentProviderKindV1::kCodex;
    message.model.clear();
    message.project_id = "project-opaque";
    message.text = "do the bounded task";
    return message;
}

std::unique_ptr<redclaw::agent::RemoteAgentBroker> make_broker(
    FakeAgentProvider** provider_out,
    redclaw::agent::RemoteAgentBrokerConfig config = {.authorized = true}) {
    auto broker = std::make_unique<redclaw::agent::RemoteAgentBroker>(
        config, std::make_unique<FakeWorkspaceManager>());
    broker->add_project({
        .project_id = "project-opaque",
        .display_name = "Project",
        .root = std::filesystem::temp_directory_path() / "redclaw-agent-fixture",
        .git_repository = true,
    });
    auto provider = std::make_unique<FakeAgentProvider>();
    *provider_out = provider.get();
    broker->add_provider(std::move(provider));
    broker->connect("peer-epoch");
    (void)broker->take_outbound();
    return broker;
}

TEST(RemoteAgentBroker, StartsOneTaskAndQueuesAtMostEight) {
    FakeAgentProvider* provider = nullptr;
    auto broker = make_broker(&provider);
    std::string error;
    ASSERT_TRUE(broker->handle_message(task_request(1, "task-1"), &error)) << error;
    EXPECT_EQ(provider->start_count, 1);
    for (std::uint64_t index = 2; index <= 9; ++index) {
        EXPECT_TRUE(broker->handle_message(
            task_request(index, "task-" + std::to_string(index)), &error)) << error;
    }
    EXPECT_EQ(broker->queued_turn_count(), 8U);
    EXPECT_FALSE(broker->handle_message(task_request(10, "task-10"), &error));
    EXPECT_NE(error.find("queue is full"), std::string::npos);
}

TEST(RemoteAgentBroker, DisconnectRejectsPendingApprovalButKeepsTask) {
    FakeAgentProvider* provider = nullptr;
    auto broker = make_broker(&provider);
    std::string error;
    ASSERT_TRUE(broker->handle_message(task_request(1, "task-1"), &error)) << error;
    provider->emit({
        .task_id = "task-1",
        .request_id = "approval-1",
        .event_kind = "command_approval",
        .text = "Run a focused test",
        .state = redclaw::protocol::AgentTaskStateV1::kAwaitingApproval,
        .approval_request = true,
    });
    broker->disconnect();
    EXPECT_EQ(provider->last_decision,
              redclaw::protocol::AgentApprovalDecisionV1::kReject);

    broker->connect("peer-epoch-2");
    // Real providers enqueue interrupt completion after disconnect() has made
    // its authoritative pause decision. It can arrive after channel reopen.
    provider->emit({
        .task_id = "task-1", .event_kind = "interrupted",
        .state = redclaw::protocol::AgentTaskStateV1::kInterrupted,
        .terminal = true,
    });
    provider->emit({
        .task_id = "task-1", .request_id = "late-approval",
        .event_kind = "command_approval",
        .state = redclaw::protocol::AgentTaskStateV1::kAwaitingApproval,
        .approval_request = true,
    });
    broker->tick();
    EXPECT_EQ(broker->metrics().task_failed_total, 0U);
    EXPECT_EQ(broker->metrics().approval_request_total, 1U);
    auto sync = redclaw::protocol::AgentMessageEnvelopeV1{};
    sync.session_epoch = "peer-epoch-2";
    sync.message_id = 1;
    sync.type = redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest;
    sync.task_id = "task-1";
    ASSERT_TRUE(broker->handle_message(sync, &error)) << error;
    const auto outbound = broker->take_outbound();
    EXPECT_TRUE(std::any_of(outbound.begin(), outbound.end(), [](const auto& item) {
        return item.type == redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot
            && item.task_state == redclaw::protocol::AgentTaskStateV1::kPaused;
    }));
    auto resume = task_request(2, "task-1");
    resume.session_epoch = "peer-epoch-2";
    resume.type = redclaw::protocol::AgentMessageTypeV1::kTurnStart;
    ASSERT_TRUE(broker->handle_message(resume, &error)) << error;
    EXPECT_EQ(provider->turn_count, 1);
    provider->emit({.task_id = "task-1", .event_kind = "running",
        .state = redclaw::protocol::AgentTaskStateV1::kRunning});
    broker->tick();
    EXPECT_EQ(broker->execution_snapshot().state,
              redclaw::protocol::AgentTaskStateV1::kRunning);
}

TEST(RemoteAgentBroker, CompletesFirstTaskThenStartsQueuedTaskWithoutDuplicateTurn) {
    FakeAgentProvider* provider = nullptr;
    auto broker = make_broker(&provider);
    std::string error;
    ASSERT_TRUE(broker->handle_message(task_request(1, "task-1"), &error)) << error;
    ASSERT_TRUE(broker->handle_message(task_request(2, "task-2"), &error)) << error;
    provider->emit({
        .task_id = "task-1",
        .event_kind = "turn_complete",
        .text = "done",
        .state = redclaw::protocol::AgentTaskStateV1::kCompleted,
        .terminal = true,
    });
    (void)broker->take_outbound();
    EXPECT_EQ(provider->start_count, 2);
    EXPECT_EQ(provider->started_task, "task-2");
}

TEST(RemoteAgentBroker, EventCacheDropsDeltasAndReportsGap) {
    FakeAgentProvider* provider = nullptr;
    redclaw::agent::RemoteAgentBrokerConfig config{
        .authorized = true,
        .max_event_bytes_per_task = 400,
        .max_total_event_bytes = 800,
    };
    auto broker = make_broker(&provider, config);
    std::string error;
    ASSERT_TRUE(broker->handle_message(task_request(1, "task-1"), &error)) << error;
    for (int index = 0; index < 10; ++index) {
        provider->emit({
            .task_id = "task-1",
            .event_kind = "text_delta",
            .text = std::string(100, static_cast<char>('a' + index)),
            .state = redclaw::protocol::AgentTaskStateV1::kRunning,
            .text_delta = true,
        });
    }
    EXPECT_LE(broker->cached_event_bytes(), 400U);
    broker->disconnect();
    broker->connect("peer-epoch-2");
    (void)broker->take_outbound();
    redclaw::protocol::AgentMessageEnvelopeV1 sync;
    sync.session_epoch = "peer-epoch-2";
    sync.message_id = 1;
    sync.type = redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest;
    sync.task_id = "task-1";
    ASSERT_TRUE(broker->handle_message(sync, &error)) << error;
    const auto outbound = broker->take_outbound();
    EXPECT_TRUE(std::any_of(outbound.begin(), outbound.end(), [](const auto& item) {
        return item.type == redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot
            && item.gap;
    }));
    const auto metrics = broker->metrics();
    EXPECT_EQ(metrics.task_create_total, 1U);
    EXPECT_EQ(metrics.event_total, 11U);
    EXPECT_GT(metrics.gap_total, 0U);
    EXPECT_GT(metrics.cached_event_bytes_peak, 0U);
}

TEST(RemoteAgentBroker, RedactsHostAbsolutePathsBeforeTransport) {
    FakeAgentProvider* provider = nullptr;
    auto broker = make_broker(&provider);
    std::string error;
    ASSERT_TRUE(broker->handle_message(task_request(1, "task-path-redaction"), &error))
        << error;
    (void)broker->take_outbound();
    provider->emit({
        .task_id = "task-path-redaction",
        .event_kind = "text_delta",
        .text = "updated C:/Users/Host/secret/project/file.txt",
        .state = redclaw::protocol::AgentTaskStateV1::kRunning,
        .text_delta = true,
    });
    const auto outbound = broker->take_outbound();
    const auto event = std::find_if(outbound.begin(), outbound.end(), [](const auto& item) {
        return item.event_kind == "text_delta";
    });
    ASSERT_NE(event, outbound.end());
    EXPECT_EQ(event->text.find("C:/Users/Host"), std::string::npos);
    EXPECT_NE(event->text.find("[HOST_PATH]"), std::string::npos);
}

TEST(RemoteAgentBroker, DecidedApprovalReplaysAsHistoryBeforeCurrentSnapshot) {
    using Type = redclaw::protocol::AgentMessageTypeV1;
    using State = redclaw::protocol::AgentTaskStateV1;
    FakeAgentProvider* provider = nullptr;
    auto broker = make_broker(&provider);
    ASSERT_TRUE(broker->handle_message(task_request(1, "task")));
    provider->emit({.task_id = "task", .request_id = "approval", .event_kind = "approval",
        .text = "Read README", .state = State::kAwaitingApproval, .approval_request = true});
    auto decision = task_request(2, "task");
    decision.type = Type::kApprovalDecision;
    decision.request_id = "approval";
    decision.approval_decision = redclaw::protocol::AgentApprovalDecisionV1::kAccept;
    ASSERT_TRUE(broker->handle_message(decision));
    provider->emit({.task_id = "task", .event_kind = "turn_completed", .text = "done",
        .state = State::kCompleted, .terminal = true});
    (void)broker->take_outbound();
    auto sync = task_request(3, "task");
    sync.type = Type::kTaskSyncRequest;
    ASSERT_TRUE(broker->handle_message(sync));
    const auto replay = broker->take_outbound();
    ASSERT_FALSE(replay.empty());
    EXPECT_TRUE(std::none_of(replay.begin(), replay.end(), [](const auto& message) { return message.type == Type::kApprovalRequest; }));
    EXPECT_TRUE(std::any_of(replay.begin(), replay.end(), [](const auto& message) { return message.event_kind == "approval_history"; }));
    EXPECT_EQ(replay.back().type, Type::kTaskSnapshot);
    EXPECT_EQ(replay.back().task_state, State::kCompleted);
    EXPECT_GT(replay.back().event_sequence, 0U);
}

TEST(RemoteAgentBroker, CriticalCacheExhaustionIsBoundedAndExplicit) {
    FakeAgentProvider* provider = nullptr;
    auto broker = make_broker(&provider, {.authorized = true,
        .max_event_bytes_per_task = 600, .max_total_event_bytes = 1000});
    std::string error;
    ASSERT_TRUE(broker->handle_message(task_request(1, "critical"), &error));
    bool failure_seen = false;
    for (int index = 0; index < 25; ++index) {
        provider->emit({.task_id = "critical", .request_id = "approval-" + std::to_string(index),
            .event_kind = "approval", .text = std::string(150, 'x'),
            .state = redclaw::protocol::AgentTaskStateV1::kAwaitingApproval, .approval_request = true});
        for (const auto& event : broker->take_outbound()) {
            failure_seen |= event.error_code == "agent_cache_exhausted";
        }
        EXPECT_LE(broker->cached_event_bytes(), 600U);
    }
    EXPECT_TRUE(failure_seen);
    EXPECT_FALSE(broker->handle_message(task_request(2, "not-started"), &error));
    EXPECT_NE(error.find("agent_busy"), std::string::npos);
}

TEST(RemoteAgentBroker, FollowUpTurnsShareTheSingleActiveTurnQueue) {
    FakeAgentProvider* provider = nullptr;
    auto broker = make_broker(&provider);
    std::string error;
    ASSERT_TRUE(broker->handle_message(task_request(1, "task-1"), &error)) << error;
    provider->emit({
        .task_id = "task-1",
        .event_kind = "turn_complete",
        .state = redclaw::protocol::AgentTaskStateV1::kCompleted,
        .terminal = true,
    });
    (void)broker->take_outbound();

    auto first_follow_up = task_request(2, "task-1");
    first_follow_up.type = redclaw::protocol::AgentMessageTypeV1::kTurnStart;
    first_follow_up.text = "first follow-up";
    ASSERT_TRUE(broker->handle_message(first_follow_up, &error)) << error;
    auto second_follow_up = task_request(3, "task-1");
    second_follow_up.type = redclaw::protocol::AgentMessageTypeV1::kTurnStart;
    second_follow_up.text = "second follow-up";
    ASSERT_TRUE(broker->handle_message(second_follow_up, &error)) << error;
    EXPECT_EQ(provider->turn_count, 1);
    EXPECT_EQ(broker->queued_turn_count(), 1U);

    provider->emit({
        .task_id = "task-1",
        .event_kind = "turn_complete",
        .state = redclaw::protocol::AgentTaskStateV1::kCompleted,
        .terminal = true,
    });
    (void)broker->take_outbound();
    EXPECT_EQ(provider->turn_count, 2);
    EXPECT_EQ(broker->queued_turn_count(), 0U);
}

TEST(RemoteAgentBroker, AgentTransportQueueIsBoundedAndReplaysRetainedOutput) {
    FakeAgentProvider* provider = nullptr;
    redclaw::agent::RemoteAgentBrokerConfig config{
        .authorized = true,
        .max_outbound_messages = 8,
    };
    auto broker = make_broker(&provider, config);
    std::string error;
    ASSERT_TRUE(broker->handle_message(task_request(1, "task-1"), &error)) << error;
    (void)broker->take_outbound();
    for (int index = 0; index < 80; ++index) {
        provider->emit({
            .task_id = "task-1",
            .event_kind = "text_delta",
            .text = "delta-" + std::to_string(index),
            .state = redclaw::protocol::AgentTaskStateV1::kRunning,
            .text_delta = true,
        });
    }
    EXPECT_LE(broker->queued_outbound_count(), 8U);
    provider->emit({
        .task_id = "task-1",
        .event_kind = "turn_complete",
        .text = "done",
        .state = redclaw::protocol::AgentTaskStateV1::kCompleted,
        .terminal = true,
    });
    broker->tick();
    broker->tick();
    std::vector<redclaw::protocol::AgentMessageEnvelopeV1> outbound;
    for (int batch = 0; batch < 20; ++batch) {
        auto messages = broker->take_outbound(8);
        outbound.insert(outbound.end(), messages.begin(), messages.end());
    }
    EXPECT_TRUE(std::any_of(outbound.begin(), outbound.end(), [](const auto& item) {
        return item.type == redclaw::protocol::AgentMessageTypeV1::kTaskComplete;
    }));
    EXPECT_FALSE(std::any_of(outbound.begin(), outbound.end(), [](const auto& item) {
        return item.type == redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot
            && item.gap && item.event_sequence > 0;
    }));
    const auto metrics = broker->metrics();
    EXPECT_EQ(metrics.task_create_total, 1U);
    EXPECT_GE(metrics.event_total, 81U);
    EXPECT_EQ(metrics.gap_total, 0U);
    EXPECT_LE(metrics.outbound_queue_peak, 8U);
}

TEST(RemoteAgentBroker, LongReplayDeliversContiguousHistoryBeforeFinalSnapshot) {
    using Type = redclaw::protocol::AgentMessageTypeV1;
    FakeAgentProvider* provider = nullptr;
    auto broker = make_broker(&provider, {.authorized = true, .max_outbound_messages = 8});
    ASSERT_TRUE(broker->handle_message(task_request(1, "long-replay")));
    (void)broker->take_outbound();
    for (int index = 0; index < 100; ++index) {
        provider->emit({.task_id = "long-replay", .event_kind = "text_delta",
            .text = "part-" + std::to_string(index),
            .state = redclaw::protocol::AgentTaskStateV1::kRunning, .text_delta = true});
        (void)broker->take_outbound();
    }
    provider->emit({.task_id = "long-replay", .event_kind = "turn_completed", .text = "answer",
        .state = redclaw::protocol::AgentTaskStateV1::kCompleted, .terminal = true});
    (void)broker->take_outbound();
    auto sync = task_request(2, "long-replay");
    sync.type = Type::kTaskSyncRequest;
    ASSERT_TRUE(broker->handle_message(sync));
    // Results handed to transport but lost on disconnect are not durable ACKs.
    (void)broker->take_outbound(3);
    broker->disconnect();
    broker->connect("reconnected-epoch");
    (void)broker->take_outbound();
    sync.session_epoch = "reconnected-epoch";
    sync.message_id = 1;
    ASSERT_TRUE(broker->handle_message(sync));
    std::uint64_t last_sequence = 0;
    bool completed = false;
    for (int batch = 0; batch < 40; ++batch) {
        const auto messages = broker->take_outbound(3);
        ASSERT_LE(messages.size(), 3U);
        for (const auto& message : messages) {
            EXPECT_FALSE(message.gap);
            if (message.type == Type::kTaskSnapshot) {
                EXPECT_EQ(last_sequence, 102U);
                EXPECT_EQ(message.event_sequence, last_sequence);
                completed = true;
            } else {
                EXPECT_EQ(message.event_sequence, ++last_sequence);
            }
        }
    }
    EXPECT_TRUE(completed);
    EXPECT_EQ(broker->metrics().gap_total, 0U);
    EXPECT_EQ(broker->metrics().replayed_event_total, 105U);
    EXPECT_LE(broker->metrics().outbound_queue_peak, 8U);
    EXPECT_EQ(provider->start_count, 1);
}

TEST(RemoteAgentBroker, OneMiBTextFloodKeepsEventAndOutboundCachesBounded) {
    FakeAgentProvider* provider = nullptr;
    constexpr std::size_t kPerTaskLimit = 128U * 1024U;
    constexpr std::size_t kTotalLimit = 256U * 1024U;
    redclaw::agent::RemoteAgentBrokerConfig config{
        .authorized = true,
        .max_event_bytes_per_task = kPerTaskLimit,
        .max_total_event_bytes = kTotalLimit,
        .max_outbound_messages = 32,
    };
    auto broker = make_broker(&provider, config);
    std::string error;
    ASSERT_TRUE(broker->handle_message(task_request(1, "task-flood"), &error)) << error;
    (void)broker->take_outbound();
    const std::string chunk(1024U, 'x');
    for (int index = 0; index < 1024; ++index) {
        provider->emit({
            .task_id = "task-flood",
            .event_kind = "text_delta",
            .text = chunk,
            .state = redclaw::protocol::AgentTaskStateV1::kRunning,
            .text_delta = true,
        });
    }
    for (int batch = 0; batch < 9; ++batch) broker->tick();
    EXPECT_LE(broker->cached_event_bytes(), kTotalLimit);
    EXPECT_LE(broker->queued_outbound_count(), 32U);
    const auto metrics = broker->metrics();
    EXPECT_GT(metrics.event_total, 1U);
    EXPECT_LE(metrics.event_total, 513U); // Provider ingress is now bounded too.
    EXPECT_GT(metrics.gap_total, 0U);
    EXPECT_LE(metrics.outbound_queue_peak, 32U);
}

TEST(RemoteAgentBroker, TerminalTurnCancelsApprovalLeaseAndLateCallbacksUntilExplicitFollowup) {
    using State = redclaw::protocol::AgentTaskStateV1;
    for (const auto terminal : {State::kCompleted, State::kFailed, State::kInterrupted}) {
        SCOPED_TRACE(static_cast<int>(terminal));
        FakeAgentProvider* provider = nullptr;
        auto broker = make_broker(&provider, {.authorized = true, .approval_timeout_ms = 20});
        ASSERT_TRUE(broker->handle_message(task_request(1, "task")));
        provider->emit({.task_id = "task", .request_id = "old-approval",
            .event_kind = "command_approval", .state = State::kAwaitingApproval,
            .approval_request = true});
        provider->emit({.task_id = "task", .event_kind = "terminal",
            .state = terminal, .terminal = true});
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        broker->tick();
        EXPECT_EQ(broker->metrics().approval_timeout_total, 0U);
        auto sync = task_request(2, "task");
        sync.type = redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest;
        ASSERT_TRUE(broker->handle_message(sync));
        const auto replies = broker->take_outbound();
        EXPECT_TRUE(std::any_of(replies.begin(), replies.end(), [terminal](const auto& reply) {
            return reply.type == redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot
                && reply.task_state == terminal;
        }));
        const auto terminal_events = broker->metrics().event_total;
        provider->emit({.task_id = "task", .request_id = "late-approval",
            .event_kind = "command_approval", .state = State::kAwaitingApproval,
            .approval_request = true});
        provider->emit({.task_id = "task", .event_kind = "duplicate-terminal",
            .state = terminal, .terminal = true});
        broker->tick();
        EXPECT_EQ(broker->metrics().event_total, terminal_events);
        EXPECT_EQ(broker->metrics().approval_request_total, 1U);
        auto followup = task_request(3, "task");
        followup.type = redclaw::protocol::AgentMessageTypeV1::kTurnStart;
        ASSERT_TRUE(broker->handle_message(followup));
        EXPECT_EQ(provider->turn_count, 1);
        provider->emit({.task_id = "task", .request_id = "new-approval",
            .event_kind = "command_approval", .state = State::kAwaitingApproval,
            .approval_request = true});
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        broker->tick();
        EXPECT_EQ(broker->metrics().approval_timeout_total, 1U);
        EXPECT_EQ(broker->metrics().approval_request_total, 2U);
    }
}

TEST(RemoteAgentBroker, ApprovalTimeoutRejectsAndPausesTask) {
    FakeAgentProvider* provider = nullptr;
    redclaw::agent::RemoteAgentBrokerConfig config{
        .authorized = true,
        .approval_timeout_ms = 1,
    };
    auto broker = make_broker(&provider, config);
    std::string error;
    ASSERT_TRUE(broker->handle_message(task_request(1, "task-1"), &error)) << error;
    provider->emit({
        .task_id = "task-1",
        .request_id = "approval-timeout",
        .event_kind = "command_approval",
        .state = redclaw::protocol::AgentTaskStateV1::kAwaitingApproval,
        .approval_request = true,
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    broker->tick();
    EXPECT_EQ(provider->last_decision,
              redclaw::protocol::AgentApprovalDecisionV1::kReject);
    const auto outbound = broker->take_outbound();
    EXPECT_TRUE(std::any_of(outbound.begin(), outbound.end(), [](const auto& item) {
        return item.event_kind == "approval_timed_out"
            && item.task_state == redclaw::protocol::AgentTaskStateV1::kPaused;
    }));
    const auto metrics = broker->metrics();
    EXPECT_EQ(metrics.approval_request_total, 1U);
    EXPECT_EQ(metrics.approval_timeout_total, 1U);
    EXPECT_EQ(metrics.approval_reject_total, 1U);
}

TEST(RemoteAgentBroker, ApprovalTimeoutReleasesSlotForNextQueuedTask) {
    FakeAgentProvider* provider = nullptr;
    redclaw::agent::RemoteAgentBrokerConfig config{
        .authorized = true,
        .approval_timeout_ms = 1,
    };
    auto broker = make_broker(&provider, config);
    std::string error;
    ASSERT_TRUE(broker->handle_message(task_request(1, "task-1"), &error)) << error;
    provider->emit({
        .task_id = "task-1",
        .request_id = "approval-timeout",
        .event_kind = "command_approval",
        .state = redclaw::protocol::AgentTaskStateV1::kAwaitingApproval,
        .approval_request = true,
    });
    ASSERT_TRUE(broker->handle_message(task_request(2, "task-2"), &error)) << error;
    ASSERT_EQ(broker->queued_turn_count(), 1U);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    broker->tick();
    (void)broker->take_outbound();

    EXPECT_EQ(provider->last_decision,
              redclaw::protocol::AgentApprovalDecisionV1::kReject);
    EXPECT_EQ(provider->start_count, 2);
    EXPECT_EQ(provider->started_task, "task-2");
    EXPECT_EQ(broker->queued_turn_count(), 0U);
}

TEST(RemoteAgentBroker, UpgradesLocalLegacyMetadataWithoutAcceptingLegacyNetworkFrames) {
    const std::string legacy = "RCD-LOCAL-AGENT-V1 UkNELUFHRU5ULVYxCnNjaGVtYV92ZXJzaW9uPTEKc2Vzc2lvbl9lcG9jaD1wZXJzaXN0ZWQtYWdlbnQtbWV0YWRhdGEKbWVzc2FnZV9pZD0xCnNlbnRfYXRfbXM9MTAwMAptZXNzYWdlX3R5cGU9YWdlbnRfdGFza19zbmFwc2hvdAp0YXNrX2lkPWxlZ2FjeS10YXNrCnByb2plY3RfaWQ9cHJvamVjdC1maXh0dXJlCnByb3ZpZGVyPWNvZGV4Cm1vZGVsPQp3b3JrX2RpcmVjdG9yeV9tb2RlPWlzb2xhdGVkX3dvcmt0cmVlCnRhc2tfc3RhdGU9Y29tcGxldGVkCmV2ZW50X3NlcXVlbmNlPTEyCnRleHQ9Cg==";
    EXPECT_FALSE(redclaw::protocol::parse_local_runtime_agent_frame_v1(legacy).ok);
    const auto path = std::filesystem::temp_directory_path()
        / ("redclaw-legacy-metadata-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    { std::ofstream file(path); file << legacy << '\n'; }
    {
        FakeAgentProvider* provider = nullptr;
        auto broker = make_broker(&provider, {.authorized = true, .metadata_path = path});
        redclaw::protocol::AgentMessageEnvelopeV1 sync;
        sync.session_epoch = "peer-epoch"; sync.message_id = 1;
        sync.type = redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest;
        sync.task_id = "legacy-task";
        ASSERT_TRUE(broker->handle_message(sync));
        const auto replies = broker->take_outbound();
        EXPECT_TRUE(std::any_of(replies.begin(), replies.end(), [](const auto& reply) {
            return reply.task_id == "legacy-task" && reply.task_state == redclaw::protocol::AgentTaskStateV1::kCompleted;
        }));
        EXPECT_EQ(provider->start_count, 0);
        // Reading history is non-mutating. Its next ordinary persistence writes
        // the new format, preserving the imported record alongside new tasks.
        ASSERT_TRUE(broker->handle_message(task_request(2, "new-task")));
        provider->emit({.task_id = "new-task", .event_kind = "turn_complete",
            .state = redclaw::protocol::AgentTaskStateV1::kCompleted, .terminal = true});
    }
    { std::ifstream file(path); std::string line; std::getline(file, line);
      EXPECT_TRUE(redclaw::protocol::parse_local_runtime_agent_frame_v1(line).ok); }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(RemoteAgentBroker, PersistsOnlyCompletedTaskMetadata) {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto metadata_path = std::filesystem::temp_directory_path()
        / ("redclaw-agent-metadata-" + unique + ".frames");
    {
        FakeAgentProvider* provider = nullptr;
        auto broker = make_broker(&provider, {
            .authorized = true,
            .metadata_path = metadata_path,
        });
        std::string error;
        ASSERT_TRUE(broker->handle_message(task_request(1, "task-persisted"), &error))
            << error;
        provider->emit({
            .task_id = "task-persisted",
            .event_kind = "turn_complete",
            .state = redclaw::protocol::AgentTaskStateV1::kCompleted,
            .terminal = true,
        });
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(metadata_path));
    {
        FakeAgentProvider* provider = nullptr;
        auto broker = make_broker(&provider, {
            .authorized = true,
            .metadata_path = metadata_path,
        });
        auto sync = redclaw::protocol::AgentMessageEnvelopeV1{};
        sync.session_epoch = "peer-epoch";
        sync.message_id = 1;
        sync.type = redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest;
        sync.task_id = "task-persisted";
        std::string error;
        ASSERT_TRUE(broker->handle_message(sync, &error)) << error;
        const auto outbound = broker->take_outbound();
        EXPECT_TRUE(std::any_of(outbound.begin(), outbound.end(), [](const auto& item) {
            return item.type == redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot
                && item.task_id == "task-persisted"
                && item.task_state == redclaw::protocol::AgentTaskStateV1::kCompleted;
        }));
    }
    std::error_code ignored;
    std::filesystem::remove(metadata_path, ignored);
}

TEST(RemoteAgentBroker, MetadataRestartResolvesLowAckWithoutReexecutingTask) {
    const auto root = std::filesystem::temp_directory_path()
        / ("redclaw-metadata-sync-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto metadata = root / "tasks.frames";
    {
        FakeAgentProvider* provider = nullptr;
        auto broker = make_broker(&provider, {.authorized = true, .metadata_path = metadata});
        ASSERT_TRUE(broker->handle_message(task_request(1, "restarted-task")));
        provider->emit({.task_id = "restarted-task", .event_kind = "turn_complete",
            .text = "Private final text", .state = redclaw::protocol::AgentTaskStateV1::kCompleted,
            .terminal = true});
        (void)broker->take_outbound();
    }
    FakeAgentProvider* provider = nullptr;
    auto broker = make_broker(&provider, {.authorized = true, .metadata_path = metadata});
    redclaw::agent::NormalAgentControlStateV1 controller({
        .journal_path = root / "controller.jsonl", .git_sha = std::string(40, 'a'),
        .executable_sha256 = std::string(64, 'b')});
    ASSERT_TRUE(controller.initialize());
    controller.request_sync("restarted-task");
    std::uint64_t next_id = 1;
    auto syncs = controller.take_sync_requests("peer-epoch", &next_id, 1000);
    ASSERT_EQ(syncs.size(), 1U);
    ASSERT_TRUE(broker->handle_message(syncs.front()));
    const auto replies = broker->take_outbound();
    ASSERT_EQ(replies.size(), 2U);
    EXPECT_TRUE(replies.front().gap);
    EXPECT_EQ(replies.front().error_code, "history_unavailable");
    EXPECT_GT(replies.front().event_sequence, 0U);
    EXPECT_EQ(replies.front().text.find("Private final text"), std::string::npos);
    for (const auto& reply : replies) ASSERT_TRUE(controller.observe_remote(reply, 2000));
    EXPECT_FALSE(controller.sync_required());
    ASSERT_TRUE(controller.task_snapshot("restarted-task").has_value());
    EXPECT_EQ(controller.task_snapshot("restarted-task")->task_state,
        redclaw::protocol::AgentTaskStateV1::kCompleted);
    EXPECT_EQ(controller.acknowledged_event_sequence("restarted-task"), replies.back().event_sequence);
    EXPECT_TRUE(controller.take_sync_requests("peer-epoch", &next_id, 8000).empty());
    // A fresh reconnect at the durable loss watermark returns state, not another gap.
    auto sync = syncs.front();
    sync.message_id = next_id++;
    sync.acknowledged_event_sequence = controller.acknowledged_event_sequence(sync.task_id);
    ASSERT_TRUE(broker->handle_message(sync));
    const auto up_to_date = broker->take_outbound();
    ASSERT_EQ(up_to_date.size(), 1U);
    EXPECT_FALSE(up_to_date.front().gap);
    EXPECT_EQ(provider->start_count, 0);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(RemoteAgentBroker, UnknownTaskSyncReturnsCorrelatedResolutionWithoutExecution) {
    FakeAgentProvider* provider = nullptr;
    auto broker = make_broker(&provider);
    redclaw::protocol::AgentMessageEnvelopeV1 sync;
    sync.session_epoch = "peer-epoch";
    sync.message_id = 1;
    sync.type = redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest;
    sync.task_id = "unknown-task";
    sync.request_id = "sync-unknown-task";
    sync.acknowledged_event_sequence = 7;
    ASSERT_TRUE(broker->handle_message(sync));
    const auto replies = broker->take_outbound();
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies.front().task_id, sync.task_id);
    EXPECT_EQ(replies.front().request_id, sync.request_id);
    EXPECT_EQ(replies.front().event_sequence, 7U);
    EXPECT_EQ(replies.front().error_code, "task_not_found");
    EXPECT_TRUE(replies.front().complete);
    EXPECT_FALSE(replies.front().gap);
    EXPECT_EQ(provider->start_count, 0);
}

}  // namespace
