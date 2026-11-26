#include "runtime/runtime_options.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace {
using redclaw::runtime::RuntimeOptions;
using redclaw::runtime::DesktopVideoMode;
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
    EXPECT_TRUE(options.gui_test_profile.empty());
    EXPECT_FALSE(options.disable_system_clipboard);
    EXPECT_EQ(options.video_mode, DesktopVideoMode::kDesktop);
    EXPECT_FALSE(options.allow_remote_input);
    EXPECT_FALSE(options.allow_remote_agent);
    EXPECT_FALSE(options.allow_remote_diagnostics);
#ifdef NDEBUG
    EXPECT_FALSE(options.enable_workspace_control);
#else
    EXPECT_TRUE(options.enable_workspace_control);
#endif
    EXPECT_FALSE(options.enable_ice_tcp);
    EXPECT_EQ(options.stream_video_max_width, 0U);
    EXPECT_FALSE(options.stream_qa_native_size);
    EXPECT_EQ(options.run_seconds, 0U);
    EXPECT_EQ(options.ice_udp_port, 55000U);
    EXPECT_EQ(options.dht_listen_port, 0U);
}
TEST(RuntimeOptions, PrivateGuiIsAnExplicitDebugControllerIndependentOfMedia) {
    RuntimeOptions options;
    std::string error;
    const bool ok = parse({"--gui-test-profile", "test-dir", "--ice-udp-port", "49176"}, &options, &error);
#if defined(_WIN32) && defined(_DEBUG)
    ASSERT_TRUE(ok) << error;
    EXPECT_TRUE(options.disable_system_clipboard);
    EXPECT_EQ(options.role, RuntimeRole::kNone);
    EXPECT_EQ(options.video_mode, DesktopVideoMode::kDesktop);
    EXPECT_FALSE(options.agent_qa_fixture_provider);
    for (const auto& [flag, mode] : std::initializer_list<std::pair<const char*, DesktopVideoMode>>{
             {"--no-desktop-video", DesktopVideoMode::kDisabled}, {"--stream-smoke", DesktopVideoMode::kSmoke},
             {"--stream-require-capture", DesktopVideoMode::kRequireCapture}}) {
        options = {};
        ASSERT_TRUE(parse({flag, "--ice-udp-port", "49176", "--gui-test-profile", "test-dir"}, &options, &error)) << error;
        EXPECT_EQ(options.video_mode, mode);
    }
#else
    EXPECT_FALSE(ok);
    EXPECT_EQ(error, "--gui-test-profile requires Windows Debug");
#endif
}

TEST(RuntimeOptions, PrivateGuiRejectsRedirectsAndOtherLaunchModesBeforeFileParsing) {
    for (const auto* option : {"--runtime-config", "--gui-maintenance-resume", "--connection-credential-file",
             "--log-dir", "--debug-control-name", "--coordination-journal-path", "--cli", "--role",
             "--gui-auto-start", "--agent-qa-fixture-provider", "--enable-port-mapping"}) {
        RuntimeOptions options;
        std::string error;
        EXPECT_FALSE(parse({option, "invalid", "--gui-test-profile", "test-dir", "--ice-udp-port", "49176"}, &options, &error));
#if defined(_WIN32) && defined(_DEBUG)
        EXPECT_EQ(error, "unsupported option for private Controller GUI") << option;
#endif
    }
    RuntimeOptions options;
    std::string error;
    EXPECT_FALSE(parse({"--gui-test-profile", "test-dir"}, &options, &error));
    options = {};
    EXPECT_FALSE(parse({"--gui-test-profile", "test-dir", "--ice-udp-port", "43000"}, &options, &error));
}

TEST(RuntimeOptions, WorkerClipboardOptOutAppliesToEitherRoleOnlyWhenRequested) {
    for (const char* role : {"host", "controller"}) {
        RuntimeOptions options;
        std::string error;
        ASSERT_TRUE(parse({"--cli", "--role", role}, &options, &error)) << error;
        EXPECT_FALSE(options.disable_system_clipboard);
        ASSERT_TRUE(parse({"--cli", "--role", role, "--no-system-clipboard"}, &options, &error)) << error;
        EXPECT_TRUE(options.disable_system_clipboard);
    }
}

TEST(RuntimeOptions, RejectsTimeLimitOverflowAtTheArgumentInEitherOrder) {
    for (const char* flag : {"--run-seconds", "--signal-timeout-seconds"}) {
        RuntimeOptions options;
        std::string error;
        EXPECT_FALSE(parse({flag, "2147483648", flag, "0"}, &options, &error));
        EXPECT_FALSE(parse({flag, "0", flag, "4294967295"}, &options, &error));
        ASSERT_TRUE(parse({flag, "2147483647"}, &options, &error)) << error;
#if defined(_WIN32) && defined(_DEBUG)
        EXPECT_FALSE(parse({flag, "2147483648", "--gui-test-profile", "test", "--ice-udp-port", "49176"}, &options, &error));
        options = {};
        EXPECT_FALSE(parse({"--gui-test-profile", "test", "--ice-udp-port", "49176", flag, "2147483648"}, &options, &error));
#endif
    }
}

TEST(RuntimeOptions, RejectsTimeLimitOverflowInProfileBeforeAnyCliOverride) {
    const auto path = std::filesystem::temp_directory_path() / ("redclaw-time-profile-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".conf");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{path};
    for (const char* key : {"run_seconds", "signal_timeout_seconds"}) {
        { std::ofstream file(path); ASSERT_TRUE(file << key << "=2147483648\n"); }
        RuntimeOptions options;
        std::string error;
        EXPECT_FALSE(parse({"--runtime-config", path.string(), "--run-seconds", "0"}, &options, &error));
        EXPECT_FALSE(parse({"--run-seconds", "0", "--runtime-config", path.string()}, &options, &error));
    }
}

TEST(RuntimeOptions, WorkspaceControlUsesBuildDefaultAndRequiresPipeNameValue) {
    RuntimeOptions options; std::string error;
    ASSERT_TRUE(parse({"--workspace-control-name", "RedClawDesktop.Test.Workspace"}, &options, &error)) << error;
#ifdef NDEBUG
    EXPECT_FALSE(options.enable_workspace_control);
#else
    EXPECT_TRUE(options.enable_workspace_control);
#endif
    EXPECT_EQ(options.workspace_control_name, "RedClawDesktop.Test.Workspace");
    options = RuntimeOptions{};
    ASSERT_TRUE(parse({"--enable-workspace-control"}, &options, &error)) << error;
    EXPECT_TRUE(options.enable_workspace_control);
    options = RuntimeOptions{};
    EXPECT_FALSE(parse({"--workspace-control-name"}, &options, &error));
}
TEST(RuntimeOptions, MaintenanceResumeStaysInGuiAndRequiresAContextArgument) {
    RuntimeOptions options; std::string error;
    EXPECT_FALSE(parse({"--gui-maintenance-resume"}, &options, &error));
    options = RuntimeOptions{};
    EXPECT_TRUE(parse({"--gui-maintenance-resume", "private-context.json"}, &options, &error)) << error;
    EXPECT_EQ(options.role, RuntimeRole::kNone); EXPECT_FALSE(options.force_cli);
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
    ASSERT_TRUE(parse({"--role", "host", "--session-code", "UNIT0001", "--signal-transport", "dht",
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
        "--signal-transport", "dht", "--session-code", "UNIT0001", "--stream-smoke",
        "--stream-require-capture", "--allow-remote-input", "--enable-debug-control"}, &gui, &error)) << error;
    EXPECT_EQ(gui.role, RuntimeRole::kNone);
    EXPECT_TRUE(gui.allow_remote_input);
    EXPECT_EQ(gui.video_mode, DesktopVideoMode::kRequireCapture);
    EXPECT_TRUE(gui.enable_debug_control);
    RuntimeOptions host;
    ASSERT_TRUE(parse({"--role", "host", "--signal-transport", "dht", "--session-code", "UNIT0001"}, &host, &error)) << error;
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

TEST(RuntimeOptions, VideoCodecDefaultsToAutoAndCliForcesH264OverProfile) {
    using redclaw::protocol::VideoCodecPreference;
    RuntimeOptions defaults;
    std::string error;
    ASSERT_TRUE(parse({}, &defaults, &error)) << error;
    EXPECT_EQ(defaults.stream_video_codec, VideoCodecPreference::kAuto);

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path()
        / ("redclaw-runtime-codec-test-" + std::to_string(nonce) + ".conf");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{path};
    { std::ofstream file(path); file << "role=host\nstream_video_codec=hevc\n"; }
    RuntimeOptions profile_only;
    ASSERT_TRUE(parse({"--runtime-config", path.string()}, &profile_only, &error)) << error;
    EXPECT_EQ(profile_only.stream_video_codec, VideoCodecPreference::kHevc);

    RuntimeOptions forced;
    ASSERT_TRUE(parse({"--stream-video-codec", "h264", "--runtime-config", path.string()}, &forced, &error)) << error;
    EXPECT_EQ(forced.stream_video_codec, VideoCodecPreference::kH264);

    RuntimeOptions invalid;
    EXPECT_FALSE(parse({"--stream-video-codec", "h265"}, &invalid, &error));
    EXPECT_NE(error.find("--stream-video-codec"), std::string::npos);
    EXPECT_FALSE(parse({"--stream-video-codec"}, &invalid, &error));

    RuntimeOptions luid;
    ASSERT_TRUE(parse({"--present-adapter-luid", "18446744073709551615"}, &luid, &error)) << error;
    EXPECT_EQ(luid.present_adapter_luid, std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(parse({"--present-adapter-luid", "12x"}, &luid, &error));
    EXPECT_FALSE(parse({"--present-adapter-luid", "18446744073709551616"}, &luid, &error));
    EXPECT_FALSE(parse({"--present-adapter-luid"}, &luid, &error));

    { std::ofstream file(path); file << "role=host\nstream_video_codec=vp9\n"; }
    EXPECT_FALSE(parse({"--runtime-config", path.string()}, &invalid, &error));
    EXPECT_NE(error.find("stream_video_codec"), std::string::npos);
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
    options.session_code = "UNIT0001";
    std::string error;
    EXPECT_TRUE(redclaw::runtime::validate_runtime_options(options, &error));
    options.dht_poll_interval_ms = 249;
    EXPECT_FALSE(redclaw::runtime::validate_runtime_options(options, &error));
    options.dht_poll_interval_ms = 250;
    options.stream_video_max_width = 319;
    EXPECT_FALSE(redclaw::runtime::validate_runtime_options(options, &error));
    options.stream_video_max_width = 0;
    options.session_code = "unit0001";
    EXPECT_FALSE(redclaw::runtime::validate_runtime_options(options, &error));
}

TEST(RuntimeOptions, ControllerFramePipeCannotBeEnabledOnHost) {
    RuntimeOptions options;
    std::string error;
    EXPECT_FALSE(parse({"--role", "host", "--stream-smoke", "--stream-frame-pipe", "test"}, &options, &error));
    options = RuntimeOptions{};
    EXPECT_TRUE(parse({"--role", "controller", "--stream-frame-pipe", "test", "--stream-navigation-pipe", "nav"}, &options, &error)) << error;
    for (const auto pipe : {"--stream-frame-pipe", "--stream-navigation-pipe"}) {
        options = {};
        EXPECT_FALSE(parse({"--role", "controller", "--no-desktop-video", pipe, "test"}, &options, &error));
    }
}

TEST(RuntimeOptions, ExplicitSessionsDefaultToVideoWithoutOptingIntoDiagnostics) {
    std::string error;
    for (const auto role : {"host", "controller"}) {
        RuntimeOptions options;
        ASSERT_TRUE(parse({"--role", role}, &options, &error)) << error;
        EXPECT_EQ(options.video_mode, DesktopVideoMode::kDesktop);
        EXPECT_FALSE(checks_video_frames(options.video_mode));
        ASSERT_TRUE(parse({"--role", role, "--no-desktop-video"}, &options, &error)) << error;
        EXPECT_EQ(options.video_mode, DesktopVideoMode::kDisabled);
    }
    for (const auto flag : {"--desktop-video", "--stream-smoke", "--stream-require-capture"}) {
        RuntimeOptions options;
        EXPECT_FALSE(parse({flag, "--no-desktop-video"}, &options, &error));
        EXPECT_FALSE(parse({"--no-desktop-video", flag}, &options, &error));
    }
    RuntimeOptions options;
    EXPECT_FALSE(parse({"--role", "controller", "--stream-qa-drop-one-media-fragment"}, &options, &error));
}

TEST(RuntimeOptions, ProfileAndCliPrecedencePreservesExplicitCaptureGates) {
    const auto path = std::filesystem::temp_directory_path() / ("redclaw-video-profile-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".conf");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); } } cleanup{path};
    auto check = [&](const char* profile, const char* flag, std::optional<DesktopVideoMode> expected) {
        SCOPED_TRACE(profile);
        SCOPED_TRACE(flag);
        { std::ofstream file(path); ASSERT_TRUE(file << "role=host\n" << profile); }
        for (bool profile_first : {true, false}) {
            RuntimeOptions options;
            std::string error;
            const bool ok = !*flag ? parse({"--runtime-config", path.string()}, &options, &error)
                : profile_first ? parse({"--runtime-config", path.string(), flag}, &options, &error)
                                : parse({flag, "--runtime-config", path.string()}, &options, &error);
            ASSERT_EQ(ok, expected.has_value()) << error;
            if (expected) EXPECT_EQ(options.video_mode, *expected);
        }
    };
    check("", "", DesktopVideoMode::kDesktop);
    check("desktop_video=false", "", DesktopVideoMode::kDisabled);
    check("stream_smoke=false", "", DesktopVideoMode::kDisabled);
    check("stream_smoke=false", "--desktop-video", DesktopVideoMode::kDesktop);
    check("desktop_video=false", "--stream-smoke", DesktopVideoMode::kSmoke);
    check("stream_smoke=true", "--no-desktop-video", DesktopVideoMode::kDisabled);
    check("stream_smoke=false\nstream_require_capture=true", "", DesktopVideoMode::kRequireCapture);
    check("stream_require_capture=true", "--no-desktop-video", std::nullopt);
    check("stream_require_capture=true", "--desktop-video", DesktopVideoMode::kRequireCapture);
    check("desktop_video=false\nstream_require_capture=true", "", std::nullopt);
    check("desktop_video=invalid", "", std::nullopt);
}

TEST(RuntimeOptions, TimedSmokeChecksAreOptInAndStrictCaptureCannotUseTransmitAlone) {
    using enum DesktopVideoMode;
    EXPECT_TRUE(timed_video_failure(kDesktop, true, false, false, false).empty());
    EXPECT_TRUE(timed_video_failure(kDisabled, false, false, false, false).empty());
    EXPECT_FALSE(timed_video_failure(kSmoke, true, false, true, false).empty());
    EXPECT_TRUE(timed_video_failure(kSmoke, true, true, false, false).empty());
    EXPECT_FALSE(timed_video_failure(kRequireCapture, true, true, false, true).empty());
    EXPECT_TRUE(timed_video_failure(kRequireCapture, true, true, true, false).empty());
    EXPECT_FALSE(timed_video_failure(kSmoke, false, true, true, false).empty());
    EXPECT_TRUE(timed_video_failure(kSmoke, false, false, false, true).empty());
}

TEST(RuntimeOptions, ReleaseStillRejectsDebugOnlyAgentFaultInjection) {
    RuntimeOptions options;
    std::string error;
    const bool accepted = parse({"--role", "controller", "--signal-transport", "dht",
        "--session-code", "UNIT0001", "--stream-smoke", "--agent-qa-force-channel-close-after-event"}, &options, &error);
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
            "--session-code", "UNIT0001", "--allow-remote-agent",
            "--enable-agent-control"}, &options, &error)) << error;
        EXPECT_TRUE(options.allow_remote_agent);
        EXPECT_TRUE(options.enable_agent_control);
    }
}
}  // namespace
