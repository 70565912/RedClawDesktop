#include "runtime/runtime_options.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace {
using redclaw::runtime::RuntimeOptions;
using redclaw::runtime::RuntimeRole;

bool parse(std::initializer_list<std::string> args, RuntimeOptions* options, std::string* error) {
    std::vector<std::string> owned{"redclaw_desktop"};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& value : owned) argv.push_back(value.data());
    return redclaw::runtime::parse_runtime_options(static_cast<int>(argv.size()), argv.data(), options, error);
}

TEST(RuntimeOptions, DefaultsRemainGuiNativeResolutionAndDenyPrivilegedCapabilities) {
    RuntimeOptions options;
    std::string error;
    ASSERT_TRUE(parse({}, &options, &error)) << error;
    EXPECT_EQ(options.role, RuntimeRole::kNone);
    EXPECT_FALSE(options.force_cli);
    EXPECT_FALSE(options.allow_remote_input);
    EXPECT_FALSE(options.allow_remote_agent);
    EXPECT_FALSE(options.allow_remote_diagnostics);
    EXPECT_FALSE(options.enable_ice_tcp);
    EXPECT_EQ(options.stream_video_max_width, 0U);
    EXPECT_FALSE(options.stream_qa_native_size);
    EXPECT_EQ(options.run_seconds, 0U);
    EXPECT_EQ(options.ice_udp_port, 55000U);
    EXPECT_EQ(options.dht_listen_port, 0U);
}

TEST(RuntimeOptions, IceUdpPortIsFixedAndRejectsAutomaticOrOverflowValues) {
    RuntimeOptions options;
    std::string error;
    ASSERT_TRUE(parse({"--ice-udp-port", "55001"}, &options, &error)) << error;
    EXPECT_EQ(options.ice_udp_port, 55001U);

    options = RuntimeOptions{};
    EXPECT_FALSE(parse({"--ice-udp-port", "0"}, &options, &error));
    options = RuntimeOptions{};
    EXPECT_FALSE(parse({"--ice-udp-port", "65536"}, &options, &error));
}

TEST(RuntimeOptions, NativeSizeQaIsExplicitAndRejectsConflictingCaps) {
    RuntimeOptions options;
    std::string error;
    ASSERT_TRUE(parse({"--role", "host", "--session-code", "RC7TST01", "--signal-transport", "dht",
        "--stream-smoke", "--stream-require-capture", "--stream-qa-native-size"}, &options, &error)) << error;
    EXPECT_TRUE(options.stream_qa_native_size);
    ASSERT_TRUE(redclaw::runtime::validate_runtime_options(options, &error)) << error;
    options.stream_video_max_width = 640;
    EXPECT_FALSE(redclaw::runtime::validate_runtime_options(options, &error));
}

TEST(RuntimeOptions, FormalHostAndGuiPassThroughKeepTheirSeparateRoles) {
    RuntimeOptions gui;
    std::string error;
    ASSERT_TRUE(parse({"--gui-role", "host", "--gui-auto-start", "--gui-persist-host-wait",
        "--signal-transport", "dht", "--session-code", "RC7TST01", "--stream-smoke",
        "--stream-require-capture", "--allow-remote-input", "--enable-debug-control"}, &gui, &error)) << error;
    EXPECT_EQ(gui.role, RuntimeRole::kNone);
    EXPECT_TRUE(gui.allow_remote_input);
    EXPECT_TRUE(gui.stream_require_capture);
    EXPECT_TRUE(gui.enable_debug_control);
    RuntimeOptions host;
    ASSERT_TRUE(parse({"--role", "host", "--signal-transport", "dht", "--session-code", "RC7TST01"}, &host, &error)) << error;
    EXPECT_EQ(host.role, RuntimeRole::kHost);
    EXPECT_EQ(host.signal_timeout_seconds, 120U);
}

TEST(RuntimeOptions, CliOverridesProfileRegardlessOfArgumentPosition) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path()
        / ("redclaw-runtime-options-test-" + std::to_string(nonce) + ".conf");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{path};
    { std::ofstream file(path); file << "role=host\nice_udp_port=55002\nstream_video_max_width=640\nallow_remote_input=false\n"; }
    RuntimeOptions options;
    std::string error;
    ASSERT_TRUE(parse({"--role", "controller", "--ice-udp-port", "55001", "--stream-video-max-width", "1280",
        "--runtime-config", path.string(), "--allow-remote-input"}, &options, &error)) << error;
    EXPECT_EQ(options.role, RuntimeRole::kController);
    EXPECT_EQ(options.ice_udp_port, 55001U);
    EXPECT_EQ(options.stream_video_max_width, 1280U);
    EXPECT_TRUE(options.allow_remote_input);
}

TEST(RuntimeOptions, HelpDoesNotStartOrValidateARuntime) {
    RuntimeOptions options;
    std::string error;
    EXPECT_TRUE(parse({"--runtime-config", "missing-profile", "--help"}, &options, &error));
    EXPECT_TRUE(options.help_requested);
    EXPECT_EQ(options.role, RuntimeRole::kNone);
}

TEST(RuntimeOptions, MalformedOrOverflowingArgumentsFailClosed) {
    for (const auto& args : std::vector<std::vector<std::string>>{
        {"--role"}, {"--role", "other"}, {"--target-port", "65536"},
        {"--run-seconds", "4294967296"}, {"--run-seconds", "-1"},
        {"--unknown"}, {"--network-bind-address", "not-an-ip"}}) {
        RuntimeOptions options;
        std::string error;
        std::vector<std::string> owned{"redclaw_desktop"};
        owned.insert(owned.end(), args.begin(), args.end());
        std::vector<char*> argv;
        for (auto& value : owned) argv.push_back(value.data());
        EXPECT_FALSE(redclaw::runtime::parse_runtime_options(static_cast<int>(argv.size()), argv.data(), &options, &error));
        EXPECT_FALSE(error.empty());
    }
}

TEST(RuntimeOptions, DhtAndViewportLimitsRemainStrict) {
    RuntimeOptions options;
    options.signal_transport = "dht";
    options.session_code = "RC7TST01";
    std::string error;
    EXPECT_TRUE(redclaw::runtime::validate_runtime_options(options, &error));
    options.dht_poll_interval_ms = 249;
    EXPECT_FALSE(redclaw::runtime::validate_runtime_options(options, &error));
    options.dht_poll_interval_ms = 250;
    options.stream_video_max_width = 319;
    EXPECT_FALSE(redclaw::runtime::validate_runtime_options(options, &error));
    options.stream_video_max_width = 0;
    options.session_code = "rc7tst01";
    EXPECT_FALSE(redclaw::runtime::validate_runtime_options(options, &error));
}

TEST(RuntimeOptions, ControllerFramePipeCannotBeEnabledOnHost) {
    RuntimeOptions options;
    std::string error;
    EXPECT_FALSE(parse({"--role", "host", "--stream-smoke", "--stream-frame-pipe", "test"}, &options, &error));
    options = RuntimeOptions{};
    EXPECT_TRUE(parse({"--role", "controller", "--stream-smoke", "--stream-frame-pipe", "test"}, &options, &error)) << error;
}

TEST(RuntimeOptions, ReleaseStillRejectsDebugOnlyAgentFaultInjection) {
    RuntimeOptions options;
    std::string error;
    const bool accepted = parse({"--role", "controller", "--signal-transport", "dht",
        "--session-code", "RC7TST01", "--stream-smoke", "--agent-qa-force-channel-close-after-event"}, &options, &error);
#ifdef NDEBUG
    EXPECT_FALSE(accepted);
#else
    EXPECT_TRUE(accepted) << error;
#endif
}

TEST(RuntimeOptions, FixtureProviderIsDebugOnlyAndOptIn) {
    RuntimeOptions options;
    EXPECT_FALSE(options.agent_qa_fixture_provider);
    std::string error;
    const bool accepted = parse({"--role", "host", "--stream-smoke",
        "--agent-qa-fixture-provider"}, &options, &error);
#ifdef NDEBUG
    EXPECT_FALSE(accepted);
#else
    EXPECT_TRUE(accepted) << error;
#endif
}

TEST(RuntimeOptions, InputDiagnosticsAreExplicitAndDoNotEnableAgentFixturesOrInputAuthorization) {
    RuntimeOptions options;
    EXPECT_FALSE(options.input_diagnostics);
    std::string error;
    const bool accepted = parse({"--role", "host", "--stream-smoke", "--log-dir", "diagnostic-test",
        "--input-diagnostics"}, &options, &error);
#ifdef NDEBUG
    EXPECT_FALSE(accepted);
#else
    ASSERT_TRUE(accepted) << error;
    EXPECT_TRUE(options.input_diagnostics);
    EXPECT_FALSE(options.agent_qa_fixture_provider);
    EXPECT_FALSE(options.allow_remote_input);
#endif
}

TEST(RuntimeOptions, AgentExecutionAuthorizationIsNotADesktopRoleRestriction) {
    for (const auto role : {"host", "controller"}) {
        RuntimeOptions options;
        std::string error;
        ASSERT_TRUE(parse({"--role", role, "--signal-transport", "dht",
            "--session-code", "RC7TST01", "--allow-remote-agent",
            "--enable-agent-control"}, &options, &error)) << error;
        EXPECT_TRUE(options.allow_remote_agent);
        EXPECT_TRUE(options.enable_agent_control);
    }
}
}  // namespace
