#include <iostream>
#include <string>

#include "redclaw/service/helper_launcher.h"
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

bool test_logon_orchestrates_helper_launch_and_sync() {
    using redclaw::service::SessionEventType;
    using redclaw::session::HandoffState;
    using redclaw::session::HandoffTransitionEvent;

    redclaw::service::SessionDetectionListener detection;
    redclaw::service::HelperProcessLauncher launcher;
    redclaw::session::SessionHandoffStateMachine handoff(HandoffState::kPreLogin);

    detection.set_session_change_handler([&](const redclaw::service::SessionChangeEvent& event) {
        if (event.type != SessionEventType::kLogon) {
            return;
        }

        const auto detected = handoff.on_event(HandoffTransitionEvent::kInteractiveSessionDetected);
        if (!detected.transitioned) {
            return;
        }

        const auto launch_result = launcher.launch_helper(make_request("m07-session-a", event.session_id));
        if (!launch_result.launched) {
            (void)handoff.on_event(HandoffTransitionEvent::kHelperLaunchFailed);
            return;
        }

        (void)handoff.on_event(HandoffTransitionEvent::kHelperLaunchSucceeded);
        (void)handoff.on_event(HandoffTransitionEvent::kCapabilitySyncSucceeded);
    });

    if (!expect_true(detection.start() == redclaw::service::SessionDetectionError::kNone, "session detection should start")) {
        return false;
    }

    detection.handle_session_change(0x5, 42);  // WTS_SESSION_LOGON

    const bool state_ok = expect_true(
        handoff.state() == HandoffState::kUserSessionActive,
        "handoff should reach user-session-active after logon flow");

    const bool helper_ok = expect_true(
        launcher.is_helper_running("m07-session-a"),
        "helper should be running after successful logon orchestration");

    std::string stop_error;
    const bool stop_ok = expect_true(
        launcher.stop_helper("m07-session-a", &stop_error),
        "helper should stop cleanly");

    const bool detection_stop_ok = expect_true(
        detection.stop() == redclaw::service::SessionDetectionError::kNone,
        "session detection should stop cleanly");

    return state_ok && helper_ok && stop_ok && detection_stop_ok;
}

bool test_logoff_orchestrates_downgrade() {
    using redclaw::service::SessionEventType;
    using redclaw::session::HandoffState;
    using redclaw::session::HandoffTransitionEvent;

    redclaw::service::SessionDetectionListener detection;
    redclaw::service::HelperProcessLauncher launcher;
    redclaw::session::SessionHandoffStateMachine handoff(HandoffState::kPreLogin);

    if (!expect_true(handoff.on_event(HandoffTransitionEvent::kInteractiveSessionDetected).transitioned, "pre-login -> launching helper")) {
        return false;
    }
    if (!expect_true(handoff.on_event(HandoffTransitionEvent::kHelperLaunchSucceeded).transitioned, "launching -> sync")) {
        return false;
    }
    if (!expect_true(handoff.on_event(HandoffTransitionEvent::kCapabilitySyncSucceeded).transitioned, "sync -> active")) {
        return false;
    }

    const auto launch_result = launcher.launch_helper(make_request("m07-session-b", 77));
    if (!expect_true(launch_result.launched, "helper should launch for logoff test")) {
        return false;
    }

    detection.set_session_change_handler([&](const redclaw::service::SessionChangeEvent& event) {
        if (event.type != SessionEventType::kLogoff) {
            return;
        }

        (void)handoff.on_event(HandoffTransitionEvent::kUserLoggedOut);
        std::string stop_error;
        (void)launcher.stop_helper("m07-session-b", &stop_error);
        (void)handoff.on_event(HandoffTransitionEvent::kDowngradeCompleted);
    });

    if (!expect_true(detection.start() == redclaw::service::SessionDetectionError::kNone, "detection starts")) {
        return false;
    }

    detection.handle_session_change(0x6, 77);  // WTS_SESSION_LOGOFF

    const bool state_ok = expect_true(
        handoff.state() == HandoffState::kPreLogin,
        "handoff should downgrade back to pre-login on logoff");

    const bool helper_ok = expect_true(
        !launcher.is_helper_running("m07-session-b"),
        "helper should not remain running after logoff downgrade");

    const bool detection_stop_ok = expect_true(
        detection.stop() == redclaw::service::SessionDetectionError::kNone,
        "detection stops");

    return state_ok && helper_ok && detection_stop_ok;
}

bool test_helper_launch_failure_moves_to_failed() {
    using redclaw::service::SessionEventType;
    using redclaw::session::HandoffState;
    using redclaw::session::HandoffTransitionEvent;

    redclaw::service::SessionDetectionListener detection;
    redclaw::service::HelperProcessLauncher launcher;
    redclaw::session::SessionHandoffStateMachine handoff(HandoffState::kPreLogin);

    detection.set_session_change_handler([&](const redclaw::service::SessionChangeEvent& event) {
        if (event.type != SessionEventType::kLogon) {
            return;
        }

        (void)handoff.on_event(HandoffTransitionEvent::kInteractiveSessionDetected);
        auto bad_request = make_request("m07-session-c", event.session_id);
        bad_request.helper_executable_path.clear();
        const auto launch_result = launcher.launch_helper(bad_request);
        if (!launch_result.launched) {
            (void)handoff.on_event(HandoffTransitionEvent::kHelperLaunchFailed);
        }
    });

    if (!expect_true(detection.start() == redclaw::service::SessionDetectionError::kNone, "detection starts")) {
        return false;
    }

    detection.handle_session_change(0x5, 91);  // WTS_SESSION_LOGON

    const bool state_ok = expect_true(
        handoff.state() == HandoffState::kFailed,
        "handoff should move to failed when helper launch fails");

    const bool reset_ok = expect_true(
        handoff.on_event(HandoffTransitionEvent::kResetToPreLogin).transitioned
            && handoff.state() == HandoffState::kPreLogin,
        "failed handoff should reset to pre-login");

    const bool detection_stop_ok = expect_true(
        detection.stop() == redclaw::service::SessionDetectionError::kNone,
        "detection stops");

    return state_ok && reset_ok && detection_stop_ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_logon_orchestrates_helper_launch_and_sync() && ok;
    ok = test_logoff_orchestrates_downgrade() && ok;
    ok = test_helper_launch_failure_moves_to_failed() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_m07_handoff_orchestration_integration_tests" << '\n';
    return 0;
}

