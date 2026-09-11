#include <iostream>
#include <string>

#include "redclaw/service/capability_registry.h"
#include "redclaw/service/capability_sync.h"
#include "redclaw/service/helper_launcher.h"
#include "redclaw/service/ipc_channel.h"
#include "redclaw/service/session_detection.h"
#include "redclaw/session/session_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

redclaw::service::HelperLaunchRequest make_request(std::string session_id, std::uint32_t user_session_id) {
    redclaw::service::HelperLaunchRequest request;
    request.session_id = std::move(session_id);
    request.user_session_id = user_session_id;
    request.helper_executable_path = "C:\\Program Files\\RedClaw\\redclaw_user_session_helper.exe";
    request.service_pipe_name = "\\\\.\\pipe\\redclaw_service_ipc";
    return request;
}

bool test_prelogin_to_user_session_handoff_e2e() {
    using redclaw::service::CapabilitySyncState;
    using redclaw::service::IpcErrorCode;
    using redclaw::service::SessionMode;
    using redclaw::session::HandoffState;
    using redclaw::session::HandoffTransitionEvent;

    redclaw::service::SessionDetectionListener detection;
    redclaw::service::HelperProcessLauncher launcher;
    redclaw::session::SessionHandoffStateMachine handoff(HandoffState::kPreLogin);

    redclaw::service::IpcChannelConfig ipc_config;
    ipc_config.pipe_name = "\\\\.\\pipe\\redclaw_handoff_integration";
    redclaw::service::WindowsNamedPipeIpcChannel channel(ipc_config);
    redclaw::service::IpcCapabilitySyncCoordinator capability_sync(channel, SessionMode::kPreLogin);

    bool sync_completed = false;
    capability_sync.set_sync_complete_handler([&](const redclaw::service::CapabilitySet&) {
        sync_completed = true;
    });

    detection.set_session_change_handler([&](const redclaw::service::SessionChangeEvent& event) {
        if (event.type != redclaw::service::SessionEventType::kLogon) {
            return;
        }

        if (!handoff.on_event(HandoffTransitionEvent::kInteractiveSessionDetected).transitioned) {
            return;
        }

        const auto launch_result = launcher.launch_helper(make_request("m07-e2e-session", event.session_id));
        if (!launch_result.launched) {
            (void)handoff.on_event(HandoffTransitionEvent::kHelperLaunchFailed);
            return;
        }

        if (!handoff.on_event(HandoffTransitionEvent::kHelperLaunchSucceeded).transitioned) {
            return;
        }

        if (!channel.connect_to_server()) {
            (void)handoff.on_event(HandoffTransitionEvent::kCapabilitySyncFailed);
            return;
        }

        const auto target_caps = redclaw::service::build_user_session_capability_set();
        if (!capability_sync.advertise_capabilities(target_caps)) {
            (void)handoff.on_event(HandoffTransitionEvent::kCapabilitySyncFailed);
            return;
        }

        const auto ack = redclaw::service::build_capability_acknowledgment(
            target_caps,
            true,
            IpcErrorCode::kNone,
            1);
        if (!capability_sync.handle_capability_acknowledgment(ack)) {
            (void)handoff.on_event(HandoffTransitionEvent::kCapabilitySyncFailed);
            return;
        }

        if (capability_sync.state() == CapabilitySyncState::kSynchronized) {
            (void)handoff.on_event(HandoffTransitionEvent::kCapabilitySyncSucceeded);
        } else {
            (void)handoff.on_event(HandoffTransitionEvent::kCapabilitySyncFailed);
        }
    });

    if (!expect_true(detection.start() == redclaw::service::SessionDetectionError::kNone, "session detection starts")) {
        return false;
    }

    detection.handle_session_change(0x5, 55);  // WTS_SESSION_LOGON

    const bool handoff_ok = expect_true(
        handoff.state() == HandoffState::kUserSessionActive,
        "handoff should reach user-session-active");
    const bool helper_ok = expect_true(
        launcher.is_helper_running("m07-e2e-session"),
        "helper should be running after handoff");
    const bool sync_ok = expect_true(
        sync_completed && capability_sync.state() == CapabilitySyncState::kSynchronized,
        "capability sync should complete successfully");
    const bool mode_ok = expect_true(
        capability_sync.current_capabilities().session_mode == SessionMode::kUserSession,
        "negotiated session mode should be user-session");

    std::string stop_error;
    const bool cleanup_ok = expect_true(
        launcher.stop_helper("m07-e2e-session", &stop_error),
        "cleanup helper stop should succeed");
    const bool detection_stop_ok = expect_true(
        detection.stop() == redclaw::service::SessionDetectionError::kNone,
        "session detection stops");

    return handoff_ok && helper_ok && sync_ok && mode_ok && cleanup_ok && detection_stop_ok;
}

bool test_user_logout_fallback_to_prelogin_e2e() {
    using redclaw::service::SessionMode;
    using redclaw::session::HandoffState;
    using redclaw::session::HandoffTransitionEvent;

    redclaw::service::SessionDetectionListener detection;
    redclaw::service::HelperProcessLauncher launcher;
    redclaw::session::SessionHandoffStateMachine handoff(HandoffState::kPreLogin);

    redclaw::service::IpcChannelConfig ipc_config;
    ipc_config.pipe_name = "\\\\.\\pipe\\redclaw_handoff_fallback";
    redclaw::service::WindowsNamedPipeIpcChannel channel(ipc_config);
    redclaw::service::IpcCapabilitySyncCoordinator capability_sync(channel, SessionMode::kPreLogin);

    // Start from already-active user session.
    (void)handoff.on_event(HandoffTransitionEvent::kInteractiveSessionDetected);
    (void)handoff.on_event(HandoffTransitionEvent::kHelperLaunchSucceeded);
    (void)handoff.on_event(HandoffTransitionEvent::kCapabilitySyncSucceeded);
    (void)launcher.launch_helper(make_request("m07-e2e-session-logout", 88));

    detection.set_session_change_handler([&](const redclaw::service::SessionChangeEvent& event) {
        if (event.type != redclaw::service::SessionEventType::kLogoff) {
            return;
        }

        (void)handoff.on_event(HandoffTransitionEvent::kUserLoggedOut);
        std::string stop_error;
        (void)launcher.stop_helper("m07-e2e-session-logout", &stop_error);

        if (channel.connect_to_server()) {
            const auto prelogin_caps = redclaw::service::build_pre_login_capability_set();
            (void)capability_sync.advertise_capabilities(prelogin_caps);
            const auto ack = redclaw::service::build_capability_acknowledgment(
                prelogin_caps,
                true,
                redclaw::service::IpcErrorCode::kNone,
                2);
            (void)capability_sync.handle_capability_acknowledgment(ack);
        }

        (void)handoff.on_event(HandoffTransitionEvent::kDowngradeCompleted);
    });

    if (!expect_true(detection.start() == redclaw::service::SessionDetectionError::kNone, "session detection starts")) {
        return false;
    }

    detection.handle_session_change(0x6, 88);  // WTS_SESSION_LOGOFF

    const bool state_ok = expect_true(
        handoff.state() == HandoffState::kPreLogin,
        "handoff should fallback to pre-login");
    const bool helper_ok = expect_true(
        !launcher.is_helper_running("m07-e2e-session-logout"),
        "helper should be stopped on logout fallback");
    const bool caps_ok = expect_true(
        capability_sync.current_capabilities().session_mode == SessionMode::kPreLogin,
        "capability sync should downgrade to pre-login");

    const bool detection_stop_ok = expect_true(
        detection.stop() == redclaw::service::SessionDetectionError::kNone,
        "session detection stops");

    return state_ok && helper_ok && caps_ok && detection_stop_ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_prelogin_to_user_session_handoff_e2e() && ok;
    ok = test_user_logout_fallback_to_prelogin_e2e() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_m07_handoff_integration_e2e_tests" << '\n';
    return 0;
}

