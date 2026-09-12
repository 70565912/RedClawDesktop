#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "redclaw/agent/agent_providers.h"

namespace {

#ifdef _WIN32
class ScopedEnvironment final {
public:
    ScopedEnvironment(const char* name, const std::string& value)
        : name_(name) {
        char* existing = nullptr;
        std::size_t existing_size = 0;
        if (_dupenv_s(&existing, &existing_size, name) == 0 && existing != nullptr) {
            previous_ = existing;
            std::free(existing);
        }
        _putenv_s(name_.c_str(), value.c_str());
    }

    ~ScopedEnvironment() {
        _putenv_s(name_.c_str(), previous_ ? previous_->c_str() : "");
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};
#endif

TEST(DebugFixtureProvider, IsExplicitlyIdentifiedAndUnavailableInRelease) {
    auto provider = redclaw::agent::make_debug_fixture_agent_provider();
#ifdef NDEBUG
    EXPECT_EQ(provider, nullptr);
#else
    ASSERT_NE(provider, nullptr);
    const auto probe = provider->probe();
    EXPECT_TRUE(probe.available);
    EXPECT_EQ(probe.version, "debug-fixture-1");
    EXPECT_TRUE(probe.requires_turn_approval);
    std::vector<redclaw::agent::AgentProviderEvent> events;
    provider->set_event_sink([&](auto event) { events.push_back(std::move(event)); });
    ASSERT_TRUE(provider->start_task({.task_id = "fixture"}, nullptr));
    ASSERT_EQ(events.size(), 1U);
    ASSERT_TRUE(events.front().approval_request);
    const auto approval_id = events.front().request_id;
    EXPECT_FALSE(provider->respond_to_approval("wrong-task", approval_id,
        redclaw::protocol::AgentApprovalDecisionV1::kAccept, nullptr));
    EXPECT_EQ(events.size(), 1U);
    ASSERT_TRUE(provider->respond_to_approval("fixture", approval_id,
        redclaw::protocol::AgentApprovalDecisionV1::kAccept, nullptr));
    std::size_t bytes = 0;
    for (const auto& event : events) if (event.text_delta) bytes += event.text.size();
    EXPECT_EQ(bytes, 1024U * 1024U);
    EXPECT_TRUE(events.back().terminal);
    EXPECT_FALSE(provider->respond_to_approval("fixture", approval_id,
        redclaw::protocol::AgentApprovalDecisionV1::kAccept, nullptr));
#endif
}

class FakeAgentProcess final : public redclaw::agent::IAgentProcess {
public:
    bool executable_available(const std::string& executable, std::string* version) override {
        if (version != nullptr) {
            *version = executable;
        }
        return executable_ready;
    }

    bool run_probe(
        const std::string& executable,
        const std::vector<std::string>& arguments,
        std::uint32_t timeout_ms,
        int* exit_code,
        std::string* output,
        std::string*) override {
        probes.push_back(executable + " " + (arguments.empty() ? "" : arguments.front()));
        probe_timeouts.push_back(timeout_ms);
        if (probe_codex_service_tier_config_failure
            && arguments.size() > 1U && arguments[0] == "login"
            && arguments[1] == "status") {
            *exit_code = 1;
            *output = "service_tier unknown variant default";
            return true;
        }
        *exit_code = probe_exit_code;
        if (!arguments.empty() && arguments.front() == "status") {
            *output = probe_status;
        } else if (!arguments.empty() && arguments.front() == "models") {
            if (models_failures_remaining > 0) {
                --models_failures_remaining;
                *exit_code = 1;
                output->clear();
                return true;
            }
            *output = probe_models;
        } else if (!arguments.empty() && arguments.front() == "login"
                   && arguments.size() > 1U && arguments[1] == "status") {
            *output = probe_status;
        } else {
            *output = executable + " fake-version";
        }
        return probe_runs;
    }

    bool start(
        const std::string& executable,
        const std::vector<std::string>& arguments,
        const std::filesystem::path& working_directory,
        redclaw::agent::AgentProcessLineCallback stdout_line,
        redclaw::agent::AgentProcessLineCallback stderr_line,
        redclaw::agent::AgentProcessExitCallback exited,
        std::string*) override {
        started_executable = executable;
        started_arguments = arguments;
        started_directory = working_directory;
        stdout_callback = std::move(stdout_line);
        stderr_callback = std::move(stderr_line);
        exit_callback = std::move(exited);
        running = start_succeeds;
        return start_succeeds;
    }

    bool write_line(const std::string& line, std::string*) override {
        std::lock_guard lock(writes_mutex);
        writes.push_back(line);
        writes_cv.notify_all();
        return write_succeeds;
    }

    bool wait_for_writes(std::size_t count) {
        std::unique_lock lock(writes_mutex);
        return writes_cv.wait_for(lock, std::chrono::seconds(2), [&] { return writes.size() >= count; });
    }

    void interrupt() override {}
    void stop() override { running = false; }

    void emit_stdout(std::string line) { stdout_callback(std::move(line)); }
    void emit_stderr(std::string line) { stderr_callback(std::move(line)); }
    void emit_exit(int code) { running = false; exit_callback(code); }

    bool executable_ready = true;
    bool probe_runs = true;
    bool probe_codex_service_tier_config_failure = false;
    int probe_exit_code = 0;
    int models_failures_remaining = 0;
    std::string probe_status = "Logged in";
    std::string probe_models = "grok-4\nclaude-sonnet";
    bool start_succeeds = true;
    bool write_succeeds = true;
    bool running = false;
    std::string started_executable;
    std::vector<std::string> started_arguments;
    std::filesystem::path started_directory;
    std::vector<std::string> probes;
    std::vector<std::uint32_t> probe_timeouts;
    std::vector<std::string> writes;
    std::mutex writes_mutex;
    std::condition_variable writes_cv;
    redclaw::agent::AgentProcessLineCallback stdout_callback;
    redclaw::agent::AgentProcessLineCallback stderr_callback;
    redclaw::agent::AgentProcessExitCallback exit_callback;
};

class EventCollector {
public:
    void push(redclaw::agent::AgentProviderEvent event) {
        std::lock_guard lock(mutex);
        events.push_back(std::move(event));
        cv.notify_all();
    }

    bool wait_for_kind(const std::string& kind) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(2), [&]() {
            return std::any_of(events.begin(), events.end(), [&](const auto& event) {
                return event.event_kind == kind;
            });
        });
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<redclaw::agent::AgentProviderEvent> events;
};

redclaw::agent::AgentProviderTaskRequest request(
    std::string task_id,
    std::string model = {}) {
    return {
        .task_id = std::move(task_id),
        .model = std::move(model),
        .working_directory = std::filesystem::temp_directory_path(),
        .instruction = "perform the fixture",
    };
}

TEST(AgentProviders, CodexUsesInitializedThreadAndTurnJsonlFlow) {
    auto process = std::make_unique<FakeAgentProcess>();
    auto* raw_process = process.get();
    auto provider = redclaw::agent::make_codex_app_server_provider(std::move(process));
    EventCollector collector;
    provider->set_event_sink([&](auto event) { collector.push(std::move(event)); });
    EXPECT_TRUE(provider->probe().available);
    std::string error;
    ASSERT_TRUE(provider->start_task(request("task-codex"), &error)) << error;
    ASSERT_GE(raw_process->writes.size(), 1U);
    EXPECT_NE(raw_process->writes[0].find("\"method\":\"initialize\""), std::string::npos);

    raw_process->emit_stdout(R"({"id":1,"result":{}})");
    ASSERT_TRUE(raw_process->wait_for_writes(3));
    ASSERT_GE(raw_process->writes.size(), 3U);
    EXPECT_NE(raw_process->writes[1].find("\"method\":\"initialized\""), std::string::npos);
    EXPECT_NE(raw_process->writes[2].find("\"method\":\"thread/start\""), std::string::npos);
    EXPECT_NE(raw_process->writes[2].find("workspace-write"), std::string::npos);
    EXPECT_NE(raw_process->writes[2].find("\"approvalPolicy\":\"untrusted\""),
        std::string::npos);
    EXPECT_EQ(raw_process->writes[2].find("unless-trusted"), std::string::npos);

    raw_process->emit_stdout(R"({"id":2,"result":{"thread":{"id":"thread-1"}}})");
    ASSERT_TRUE(raw_process->wait_for_writes(4));
    ASSERT_GE(raw_process->writes.size(), 4U);
    EXPECT_NE(raw_process->writes[3].find("\"method\":\"turn/start\""), std::string::npos);
    raw_process->emit_stdout(
        R"({"method":"item/agentMessage/delta","params":{"delta":"progress"}})");
    raw_process->emit_stdout(
        R"({"method":"turn/completed","params":{"turn":{"id":"turn-1"}}})");
    EXPECT_TRUE(collector.wait_for_kind("turn_completed"));
    provider->shutdown();
}

TEST(AgentProviders, CursorRequiresReadinessAndPerTurnApproval) {
    auto process = std::make_unique<FakeAgentProcess>();
    auto* raw_process = process.get();
    auto provider = redclaw::agent::make_cursor_agent_provider(std::move(process));
    EventCollector collector;
    provider->set_event_sink([&](auto event) { collector.push(std::move(event)); });
    const auto capability = provider->probe();
    ASSERT_TRUE(capability.available);
    EXPECT_TRUE(capability.requires_turn_approval);
    std::string error;
    ASSERT_TRUE(provider->start_task(request("task-cursor", "grok-4"), &error)) << error;
    ASSERT_TRUE(collector.wait_for_kind("cursor_turn_preapproval"));
    std::string approval_id;
    {
        std::lock_guard lock(collector.mutex);
        approval_id = collector.events.back().request_id;
    }
    ASSERT_TRUE(provider->respond_to_approval(
        "task-cursor", approval_id,
        redclaw::protocol::AgentApprovalDecisionV1::kAccept, &error)) << error;
    EXPECT_EQ(raw_process->started_executable, "cursor-agent");
    EXPECT_NE(std::find(raw_process->started_arguments.begin(), raw_process->started_arguments.end(),
                        "stream-json"), raw_process->started_arguments.end());
    EXPECT_NE(std::find(raw_process->started_arguments.begin(), raw_process->started_arguments.end(),
                        "--trust"), raw_process->started_arguments.end());
    EXPECT_NE(std::find(raw_process->started_arguments.begin(), raw_process->started_arguments.end(),
                        "--force"), raw_process->started_arguments.end());
    EXPECT_EQ(std::find(raw_process->started_arguments.begin(), raw_process->started_arguments.end(),
                        "--yolo"), raw_process->started_arguments.end());
    raw_process->emit_stdout(R"({"type":"assistant","chat_id":"chat-1","text":"done"})");
    raw_process->emit_exit(0);
    EXPECT_TRUE(collector.wait_for_kind("turn_completed"));
    provider->shutdown();
}

TEST(AgentProviders, ProviderParsersRejectMalformedJsonWithoutThrowing) {
    {
        auto process = std::make_unique<FakeAgentProcess>();
        auto* raw_process = process.get();
        auto provider = redclaw::agent::make_codex_app_server_provider(std::move(process));
        EventCollector collector;
        provider->set_event_sink([&](auto event) { collector.push(std::move(event)); });
        ASSERT_TRUE(provider->probe().available);
        std::string error;
        ASSERT_TRUE(provider->start_task(request("task-codex-invalid-json"), &error))
            << error;
        raw_process->emit_stdout("{");
        ASSERT_TRUE(collector.wait_for_kind("provider_protocol_error"));
        provider->shutdown();
    }

    {
        auto process = std::make_unique<FakeAgentProcess>();
        auto* raw_process = process.get();
        auto provider = redclaw::agent::make_cursor_agent_provider(std::move(process));
        EventCollector collector;
        provider->set_event_sink([&](auto event) { collector.push(std::move(event)); });
        ASSERT_TRUE(provider->probe().available);
        std::string error;
        ASSERT_TRUE(provider->start_task(
            request("task-cursor-invalid-json", "grok-4"), &error)) << error;
        ASSERT_TRUE(collector.wait_for_kind("cursor_turn_preapproval"));
        std::string approval_id;
        {
            std::lock_guard lock(collector.mutex);
            approval_id = collector.events.back().request_id;
        }
        ASSERT_TRUE(provider->respond_to_approval(
            "task-cursor-invalid-json",
            approval_id,
            redclaw::protocol::AgentApprovalDecisionV1::kAccept,
            &error)) << error;
        raw_process->emit_stdout("{");
        ASSERT_TRUE(collector.wait_for_kind("provider_protocol_error"));
        provider->shutdown();
    }
}

TEST(AgentProviders, CursorDoesNotAdvertiseWhenAccountProbeFails) {
    auto process = std::make_unique<FakeAgentProcess>();
    process->probe_exit_code = 1;
    auto provider = redclaw::agent::make_cursor_agent_provider(std::move(process));
    const auto capability = provider->probe();
    EXPECT_FALSE(capability.available);
    EXPECT_NE(capability.unavailable_reason.find("readiness"), std::string::npos);
    provider->shutdown();
}

TEST(AgentProviders, CursorDoesNotAdvertiseWhenStatusSaysNotLoggedIn) {
    auto process = std::make_unique<FakeAgentProcess>();
    process->probe_status = "Not logged in";
    auto provider = redclaw::agent::make_cursor_agent_provider(std::move(process));
    const auto capability = provider->probe();
    EXPECT_FALSE(capability.available);
    EXPECT_NE(capability.unavailable_reason.find("not authenticated"), std::string::npos);
    provider->shutdown();
}

TEST(AgentProviders, CursorPublishesOnlyAvailableGrokModelsAndRefreshes) {
    auto process = std::make_unique<FakeAgentProcess>();
    auto* raw_process = process.get();
    process->probe_models =
        "claude-sonnet - Claude Sonnet\n"
        "- grok-4-fast\n"
        "grok-4-fast - Cursor Grok 4 Fast\n";
    auto provider = redclaw::agent::make_cursor_agent_provider(std::move(process));
    const auto ready = provider->probe();
    ASSERT_TRUE(ready.available);
    ASSERT_EQ(ready.models.size(), 1U);
    EXPECT_EQ(ready.models.front(), "grok-4-fast");
    raw_process->probe_status = "Not logged in";
    const auto refreshed = provider->probe(true);
    EXPECT_FALSE(refreshed.available);
    EXPECT_EQ(refreshed.readiness,
        redclaw::protocol::AgentProviderReadinessV1::kNotAuthenticated);
    provider->shutdown();
}

TEST(AgentProviders, CursorRetriesOneTransientModelProbeWithinBoundedTimeout) {
    auto process = std::make_unique<FakeAgentProcess>();
    auto* raw_process = process.get();
    process->models_failures_remaining = 1;
    process->probe_models = "cursor-grok-4.6-high-fast - Cursor Grok 4.6 Fast\n";
    auto provider = redclaw::agent::make_cursor_agent_provider(std::move(process));
    const auto ready = provider->probe();
    ASSERT_TRUE(ready.available);
    ASSERT_EQ(ready.models.size(), 1U);
    EXPECT_EQ(ready.models.front(), "cursor-grok-4.6-high-fast");
    EXPECT_EQ(std::count(raw_process->probes.begin(), raw_process->probes.end(),
                  "cursor-agent models"), 2);
    EXPECT_GE(std::count(raw_process->probe_timeouts.begin(),
                  raw_process->probe_timeouts.end(), 30000U), 2);
    provider->shutdown();
}

TEST(AgentProviders, CodexRequiresAuthenticationAndRefreshes) {
    auto process = std::make_unique<FakeAgentProcess>();
    auto* raw_process = process.get();
    auto provider = redclaw::agent::make_codex_app_server_provider(std::move(process));
    EXPECT_TRUE(provider->probe().available);
    raw_process->probe_status = "Not logged in";
    raw_process->probe_exit_code = 1;
    const auto refreshed = provider->probe(true);
    EXPECT_FALSE(refreshed.available);
    EXPECT_EQ(refreshed.readiness,
        redclaw::protocol::AgentProviderReadinessV1::kNotAuthenticated);
    provider->shutdown();
}

TEST(AgentProviders, CodexUsesScopedFastCompatibilityForUnsupportedServiceTierConfig) {
    auto process = std::make_unique<FakeAgentProcess>();
    auto* raw_process = process.get();
    raw_process->probe_codex_service_tier_config_failure = true;
    auto provider = redclaw::agent::make_codex_app_server_provider(std::move(process));
    ASSERT_TRUE(provider->probe().available);
    std::string error;
    ASSERT_TRUE(provider->start_task(request("task-codex-compat"), &error)) << error;
    ASSERT_GE(raw_process->started_arguments.size(), 5U);
    EXPECT_EQ(raw_process->started_arguments[0], "-c");
    EXPECT_EQ(raw_process->started_arguments[1], "service_tier=\"fast\"");
    EXPECT_EQ(raw_process->started_arguments[2], "app-server");
    provider->shutdown();
}

#ifdef _WIN32
TEST(AgentProcess, PrefersCodexDesktopCliBeforePowerShellFallback) {
    const auto unique_suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto fixture_directory = std::filesystem::temp_directory_path()
        / ("redclaw-codex-discovery-" + unique_suffix);
    const auto path_directory = fixture_directory / "path";
    const auto local_app_data = fixture_directory / "local";
    const auto desktop_bin = local_app_data / "OpenAI" / "Codex" / "bin" / "current";
    std::filesystem::create_directories(path_directory);
    std::filesystem::create_directories(desktop_bin);
    {
        std::ofstream script(path_directory / "codex.ps1", std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(script.is_open());
        script << "exit 0\n";
    }
    const auto desktop_cli = desktop_bin / "codex.exe";
    {
        std::ofstream executable(desktop_cli, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(executable.is_open());
        executable << "fixture";
    }

    ScopedEnvironment path("PATH", path_directory.string());
    ScopedEnvironment local("LOCALAPPDATA", local_app_data.string());
    redclaw::agent::AgentResolvedCommand resolved;
    std::string error;
    ASSERT_TRUE(redclaw::agent::resolve_agent_command(
        "codex", {"login"}, &resolved, &error)) << error;
    EXPECT_EQ(resolved.target, desktop_cli);
    EXPECT_EQ(resolved.application, desktop_cli);
    ASSERT_EQ(resolved.arguments.size(), 1U);
    EXPECT_EQ(resolved.arguments.front(), "login");

    std::error_code cleanup_error;
    std::filesystem::remove_all(fixture_directory, cleanup_error);
}

TEST(AgentProviders, RealProviderReadinessWhenExplicitlyEnabled) {
    const char* selected = std::getenv("REDCLAW_REAL_AGENT_PROVIDER");
    if (selected == nullptr) {
        GTEST_SKIP() << "set REDCLAW_REAL_AGENT_PROVIDER=codex|cursor for the local readiness gate";
    }
    const std::string provider_name(selected);
    std::unique_ptr<redclaw::agent::IAgentProvider> provider;
    if (provider_name == "codex") {
        provider = redclaw::agent::make_codex_app_server_provider();
    } else if (provider_name == "cursor") {
        provider = redclaw::agent::make_cursor_agent_provider();
    } else {
        FAIL() << "REDCLAW_REAL_AGENT_PROVIDER must be codex or cursor";
    }
    const auto readiness = provider->probe(true);
    EXPECT_EQ(readiness.readiness,
        redclaw::protocol::AgentProviderReadinessV1::kReady)
        << "reason=" << readiness.unavailable_reason;
    if (provider_name == "cursor") {
        EXPECT_FALSE(readiness.models.empty());
        EXPECT_TRUE(std::all_of(
            readiness.models.begin(), readiness.models.end(), [](const auto& model) {
                return model.find("grok") != std::string::npos
                    || model.find("Grok") != std::string::npos;
            }));
    }
    provider->shutdown();
}

TEST(AgentProcess, WindowsRunsPowerShellLauncherWhenExeIsAbsent) {
    const auto unique_suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto fixture_directory = std::filesystem::temp_directory_path()
        / ("redclaw-agent-process-" + unique_suffix);
    std::filesystem::create_directories(fixture_directory);
    const auto launcher_base = fixture_directory / "fixture-agent";
    const auto launcher = launcher_base.string() + ".ps1";
    {
        std::ofstream script(launcher, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(script.is_open());
        script << "Write-Output ('probe:' + $args[0])\nexit 7\n";
    }

    auto process = redclaw::agent::make_system_agent_process();
    std::string resolved;
    EXPECT_TRUE(process->executable_available(launcher_base.string(), &resolved));
    EXPECT_NE(resolved.find("fixture-agent.ps1"), std::string::npos);
    int exit_code = 0;
    std::string output;
    std::string error;
    EXPECT_TRUE(process->run_probe(
        launcher_base.string(), {"ready"}, 5000,
        &exit_code, &output, &error)) << error;
    EXPECT_EQ(exit_code, 7);
    EXPECT_NE(output.find("probe:ready"), std::string::npos);

    std::error_code cleanup_error;
    std::filesystem::remove_all(fixture_directory, cleanup_error);
}

TEST(AgentProcess, WindowsProbeDrainsLargeOutputBeforeChildExit) {
    const auto unique_suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto fixture_directory = std::filesystem::temp_directory_path()
        / ("redclaw-agent-probe-flood-" + unique_suffix);
    std::filesystem::create_directories(fixture_directory);
    const auto launcher_base = fixture_directory / "fixture-flood-agent";
    const auto launcher = launcher_base.string() + ".ps1";
    {
        std::ofstream script(launcher, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(script.is_open());
        script << "1..2000 | ForEach-Object { Write-Output ('grok-fixture-' + $_ + ('x' * 32)) }\n"
                  "exit 0\n";
    }
    auto process = redclaw::agent::make_system_agent_process();
    int exit_code = 1;
    std::string output;
    std::string error;
    EXPECT_TRUE(process->run_probe(
        launcher_base.string(), {}, 10000,
        &exit_code, &output, &error)) << error;
    EXPECT_EQ(exit_code, 0);
    EXPECT_NE(output.find("grok-fixture-1"), std::string::npos);
    EXPECT_LE(output.size(), 64U * 1024U);
    std::error_code cleanup_error;
    std::filesystem::remove_all(fixture_directory, cleanup_error);
}
#endif

}  // namespace
