#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "redclaw/agent/agent_executor.h"
#include "redclaw/agent/local_agent_pipe.h"
#include "redclaw/agent/bounded_agent_events.h"
#include "redclaw/agent/agent_providers.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using namespace std::chrono_literals;
using Message = redclaw::protocol::AgentMessageEnvelopeV1;
using Type = redclaw::protocol::AgentMessageTypeV1;
bool wait_until(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
    return condition();
}
Message message(std::uint64_t id = 1) {
    Message value;
    value.session_epoch = "isolation-test";
    value.message_id = id;
    value.type = Type::kTaskSyncRequest;
    value.request_id = "sync-" + std::to_string(id);
    return value;
}

struct BlockState {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};
class BlockingProvider final : public redclaw::agent::IAgentProvider {
public:
    explicit BlockingProvider(std::shared_ptr<BlockState> state) : state_(std::move(state)) {}
    redclaw::agent::AgentProviderProbe probe(bool) override {
        std::unique_lock lock(state_->mutex);
        state_->entered = true;
        state_->changed.notify_all();
        state_->changed.wait(lock, [&] { return state_->released; });
        return {.provider = redclaw::protocol::AgentProviderKindV1::kCodex};
    }
    void set_event_sink(redclaw::agent::AgentProviderEventSink) override {}
    bool start_task(const redclaw::agent::AgentProviderTaskRequest&, std::string*) override { return true; }
    bool resume_task(const redclaw::agent::AgentProviderTaskRequest&, std::string*) override { return true; }
    bool start_turn(const std::string&, const std::string&, std::string*) override { return true; }
    bool steer(const std::string&, const std::string&, std::string*) override { return true; }
    bool interrupt(const std::string&, std::string*) override { return true; }
    bool respond_to_approval(const std::string&, const std::string&,
        redclaw::protocol::AgentApprovalDecisionV1, std::string*) override { return true; }
    void shutdown() override {}
private:
    std::shared_ptr<BlockState> state_;
};

TEST(AgentIsolation, BlockedProviderDoesNotBlockDesktopCallerOrSnapshots) {
    auto state = std::make_shared<BlockState>();
    auto broker = std::make_unique<redclaw::agent::RemoteAgentBroker>(
        redclaw::agent::RemoteAgentBrokerConfig{.authorized = true});
    broker->add_provider(std::make_unique<BlockingProvider>(state));
    redclaw::agent::AgentExecutor executor(std::move(broker));
    executor.connect("isolation-test");
    const bool entered = wait_until([&] { std::lock_guard lock(state->mutex); return state->entered; });
    EXPECT_TRUE(entered);
    const auto started = std::chrono::steady_clock::now();
    for (std::uint64_t id = 1; id <= 512; ++id) {
        std::string error;
        const bool queued = executor.handle_message(message(id), &error);
        if (id > 128) EXPECT_FALSE(queued);
        EXPECT_LE(executor.snapshot().queued_commands, 128U);
        (void)executor.take_outbound();
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started, 250ms);
    executor.disconnect(); // Coalesced lifecycle command must also return immediately.
    {
        std::lock_guard lock(state->mutex);
        state->released = true;
    }
    state->changed.notify_all();
    EXPECT_GE(executor.snapshot().rejected_total, 384U);
}

TEST(AgentIsolation, ProviderCriticalOverflowIsBoundedAndExplicit) {
    redclaw::agent::BoundedAgentEvents events;
    for (int index = 0; index < 2000; ++index) {
        events.push_back({.task_id = "task", .request_id = std::to_string(index),
            .event_kind = "approval", .text = std::string(8192, 'x'), .approval_request = true});
    }
    EXPECT_TRUE(events.failed());
    int count = 0;
    bool explicit_error = false;
    while (!events.empty()) {
        auto event = events.take_front();
        explicit_error |= event.error_code == "agent_queue_exhausted";
        ++count;
    }
    EXPECT_LE(count, 513);
    EXPECT_TRUE(explicit_error);
}

TEST(AgentIsolation, SixtySecondProviderWaitKeepsCallerResponsive) {
    auto state = std::make_shared<BlockState>();
    auto broker = std::make_unique<redclaw::agent::RemoteAgentBroker>(
        redclaw::agent::RemoteAgentBrokerConfig{.authorized = true});
    broker->add_provider(std::make_unique<BlockingProvider>(state));
    redclaw::agent::AgentExecutor executor(std::move(broker));
    executor.connect("sixty-second-wait");
    EXPECT_TRUE(wait_until([&] { std::lock_guard lock(state->mutex); return state->entered; }));
    const auto started = std::chrono::steady_clock::now();
    auto previous = started;
    auto maximum_gap = std::chrono::steady_clock::duration::zero();
    auto maximum_call = std::chrono::steady_clock::duration::zero();
    std::uint64_t ticks = 0;
    while (std::chrono::steady_clock::now() - started < std::chrono::seconds(60)) {
        const auto call_started = std::chrono::steady_clock::now();
        (void)executor.snapshot();
        (void)executor.take_outbound();
        const auto now = std::chrono::steady_clock::now();
        maximum_call = std::max(maximum_call, now - call_started);
        maximum_gap = std::max(maximum_gap, now - previous);
        previous = now;
        ++ticks;
        std::this_thread::sleep_for(2ms);
    }
    { std::lock_guard lock(state->mutex); state->released = true; }
    state->changed.notify_all();
    EXPECT_GT(ticks, 1000U);
    // Assert the production API boundary, not the OS wake-up after sleep_for.
    // Keep scheduling gaps as evidence: a descheduled test thread is not proof
    // that the provider worker held the caller's mutex for that interval.
    EXPECT_LT(maximum_call, 250ms);
    RecordProperty("provider_wait_ms", 60000);
    RecordProperty("caller_max_call_ms", std::chrono::duration_cast<std::chrono::milliseconds>(maximum_call).count());
    RecordProperty("caller_max_gap_ms", std::chrono::duration_cast<std::chrono::milliseconds>(maximum_gap).count());
}

#ifdef _WIN32
std::string pipe_name() {
    static std::atomic<std::uint64_t> serial{0};
    return "RedClawAgent-Test-" + std::to_string(GetCurrentProcessId()) + "-"
        + std::to_string(GetTickCount64()) + "-" + std::to_string(++serial);
}

TEST(AgentLocalPipeIntegration, BidirectionalFramesAndCancellationWithoutStdio) {
    redclaw::agent::LocalAgentPipe server, client;
    const auto name = pipe_name();
    std::string error;
    ASSERT_TRUE(server.start(name, true, GetCurrentProcessId(), &error)) << error;
    ASSERT_TRUE(client.start(name, false, GetCurrentProcessId(), &error)) << error;
    ASSERT_TRUE(wait_until([&] { return server.stats().connected && client.stats().connected; }));
    auto large = message();
    large.text.assign(8000, 'x');
    ASSERT_TRUE(client.send(large, &error)) << error;
    ASSERT_TRUE(server.send(message(2), &error)) << error;
    ASSERT_TRUE(wait_until([&] { return server.stats().received_total == 1 && client.stats().received_total == 1; }));
    EXPECT_EQ(server.take_received().front().text, large.text);
    EXPECT_EQ(client.take_received().front().message_id, 2U);
    const auto started = std::chrono::steady_clock::now();
    server.stop();
    client.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 250ms);
}

TEST(AgentLocalPipeIntegration, WrongExpectedPidFailsClosed) {
    redclaw::agent::LocalAgentPipe server, client;
    const auto name = pipe_name();
    ASSERT_TRUE(server.start(name, true, GetCurrentProcessId() + 1));
    ASSERT_TRUE(client.start(name, false, GetCurrentProcessId()));
    ASSERT_TRUE(wait_until([&] { return !server.stats().error.empty(); }));
    EXPECT_FALSE(server.stats().connected);
    EXPECT_FALSE(server.send(message()));
}

TEST(AgentLocalPipeIntegration, SimultaneousLargeWritesAndFullReceiveQueueDoNotDisconnect) {
    redclaw::agent::LocalAgentPipe server, client;
    const auto name = pipe_name();
    ASSERT_TRUE(server.start(name, true, GetCurrentProcessId()));
    ASSERT_TRUE(client.start(name, false, GetCurrentProcessId()));
    ASSERT_TRUE(wait_until([&] { return server.stats().connected && client.stats().connected; }));
    auto large = message();
    large.text.assign(8000, 'x');
    for (int i = 0; i < 100; ++i) {
        large.message_id = i + 1;
        ASSERT_TRUE(server.send(large));
        ASSERT_TRUE(client.send(large));
    }
    ASSERT_TRUE(wait_until([&] { return server.stats().received_total == 100 && client.stats().received_total == 100; }));
    for (int i = 0; i < 60; ++i) {
        large.message_id = i + 101;
        ASSERT_TRUE(server.send(large));
    }
    ASSERT_TRUE(wait_until([&] { return client.stats().received_depth == 128; }));
    EXPECT_TRUE(client.stats().connected);
    EXPECT_EQ(client.stats().connection_total, 1U);
    EXPECT_EQ(client.stats().rejected_total, 0U);
    EXPECT_EQ(client.take_received(128).size(), 128U);
    ASSERT_TRUE(wait_until([&] { return client.stats().received_total == 160; }));
    EXPECT_EQ(client.take_received(128).size(), 32U);
    EXPECT_EQ(client.stats().connection_total, 1U);
}

TEST(AgentLocalPipeIntegration, SameIdentityReconnectsWithoutDesktopRebuild) {
    redclaw::agent::LocalAgentPipe server, client;
    const auto name = pipe_name();
    ASSERT_TRUE(server.start(name, true, GetCurrentProcessId()));
    ASSERT_TRUE(client.start(name, false, GetCurrentProcessId()));
    ASSERT_TRUE(wait_until([&] { return server.stats().connected && client.stats().connected; }));
    client.stop();
    ASSERT_TRUE(wait_until([&] { return !server.stats().connected; }));
    ASSERT_TRUE(client.start(name, false, GetCurrentProcessId()));
    ASSERT_TRUE(wait_until([&] { return server.stats().connection_total == 2 && client.stats().connected; }));
    ASSERT_TRUE(client.send(message(3)));
    ASSERT_TRUE(wait_until([&] { return server.stats().received_total == 1; }));
    EXPECT_EQ(server.take_received().front().message_id, 3U);
}

TEST(AgentLocalPipeIntegration, ContinuousProbeOutputCannotExtendDeadline) {
    auto process = redclaw::agent::make_system_agent_process();
    int code = 0;
    std::string output, error;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(process->run_probe("powershell.exe", {"-NoProfile", "-NonInteractive", "-Command",
        "while ($true) { [Console]::Out.WriteLine('probe-output') }"}, 500, &code, &output, &error));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2500ms);
    EXPECT_LE(output.size(), 64U * 1024U);
}

TEST(AgentLocalPipeIntegration, OversizeProviderRecordFailsOnlyOwnedProcess) {
    auto process = redclaw::agent::make_system_agent_process();
    std::atomic<int> exited{-1};
    std::atomic<int> lines{0};
    std::string error;
    ASSERT_TRUE(process->start("powershell.exe",
        {"-NoProfile", "-Command", "[Console]::Write(('x' * 300000)); Start-Sleep -Seconds 30"},
        std::filesystem::current_path(), [&](std::string) { ++lines; }, {},
        [&](int code) { exited.store(code); }, &error)) << error;
    EXPECT_TRUE(wait_until([&] { return exited.load() != -1; }, 5000ms));
    EXPECT_EQ(exited.load(), 125);
    EXPECT_EQ(lines.load(), 0);
    process->stop();
}

TEST(AgentLocalPipeIntegration, ProviderStdinWriteHasFiveSecondDeadline) {
    auto process = redclaw::agent::make_system_agent_process();
    std::string error;
    ASSERT_TRUE(process->start("powershell.exe", {"-NoProfile", "-NonInteractive", "-Command",
        "Start-Sleep -Seconds 30"}, {}, [](std::string) {}, [](std::string) {}, [](int) {}, &error)) << error;
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(process->write_line(std::string(128U * 1024U, 'x'), &error));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_GE(elapsed, 4500ms);
    EXPECT_LT(elapsed, 6500ms);
    EXPECT_NE(error.find("5 seconds"), std::string::npos);
    process->stop();
}

TEST(AgentLocalPipeIntegration, PartialHeaderCoalescedFramesAndOversizeRejected) {
    redclaw::agent::LocalAgentPipe server;
    const auto name = pipe_name();
    ASSERT_TRUE(server.start(name, true, GetCurrentProcessId()));
    const auto path = L"\\\\.\\pipe\\" + std::wstring(name.begin(), name.end());
    HANDLE pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(pipe, INVALID_HANDLE_VALUE);
    EXPECT_TRUE(wait_until([&] { return server.stats().connected; }));
    std::string payload = redclaw::protocol::serialize_agent_message_v1(message());
    std::string frame(4, '\0');
    for (unsigned int i = 0; i < 4; ++i) frame[i] = static_cast<char>((payload.size() >> (8 * i)) & 255U);
    frame += payload;
    DWORD written = 0;
    EXPECT_TRUE(WriteFile(pipe, frame.data(), 2, &written, nullptr));
    std::this_thread::sleep_for(10ms);
    EXPECT_EQ(server.stats().received_total, 0U);
    auto tail = frame.substr(2) + frame;
    EXPECT_TRUE(WriteFile(pipe, tail.data(), static_cast<DWORD>(tail.size()), &written, nullptr));
    EXPECT_TRUE(wait_until([&] { return server.stats().received_total == 2; }));
    const std::array<unsigned char, 4> oversize{1, 0, 1, 0};
    EXPECT_TRUE(WriteFile(pipe, oversize.data(), 4, &written, nullptr));
    EXPECT_TRUE(wait_until([&] { return !server.stats().connected; }));
    EXPECT_NE(server.stats().error.find("limit"), std::string::npos);
    CloseHandle(pipe);
}
#endif
}  // namespace
