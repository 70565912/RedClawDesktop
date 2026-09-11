#include <iostream>
#include <string>
#include <vector>

#include "redclaw/session/session_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_handoff_happy_path() {
    using redclaw::session::HandoffState;
    using redclaw::session::HandoffTransitionEvent;
    using redclaw::session::SessionHandoffStateMachine;

    SessionHandoffStateMachine sm(HandoffState::kPreLogin);
    auto step = sm.on_event(HandoffTransitionEvent::kInteractiveSessionDetected);
    if (!expect_true(step.transitioned && step.state == HandoffState::kLaunchingHelper, "pre-login -> launching-helper")) {
        return false;
    }

    step = sm.on_event(HandoffTransitionEvent::kHelperLaunchSucceeded);
    if (!expect_true(step.transitioned && step.state == HandoffState::kSynchronizingCapabilities, "launching -> sync")) {
        return false;
    }

    step = sm.on_event(HandoffTransitionEvent::kCapabilitySyncSucceeded);
    return expect_true(step.transitioned && step.state == HandoffState::kUserSessionActive, "sync -> active");
}

bool test_logout_downgrade_path() {
    using redclaw::session::HandoffState;
    using redclaw::session::HandoffTransitionEvent;
    using redclaw::session::SessionHandoffStateMachine;

    SessionHandoffStateMachine sm(HandoffState::kUserSessionActive);
    auto step = sm.on_event(HandoffTransitionEvent::kUserLoggedOut);
    if (!expect_true(step.transitioned && step.state == HandoffState::kDowngrading, "active -> downgrading on logout")) {
        return false;
    }

    step = sm.on_event(HandoffTransitionEvent::kDowngradeCompleted);
    return expect_true(step.transitioned && step.state == HandoffState::kPreLogin, "downgrading -> pre-login");
}

bool test_helper_lost_path() {
    using redclaw::session::HandoffState;
    using redclaw::session::HandoffTransitionEvent;
    using redclaw::session::SessionHandoffStateMachine;

    SessionHandoffStateMachine sm(HandoffState::kSynchronizingCapabilities);
    auto step = sm.on_event(HandoffTransitionEvent::kHelperLost);
    if (!expect_true(step.transitioned && step.state == HandoffState::kDowngrading, "sync -> downgrading on helper loss")) {
        return false;
    }

    step = sm.on_event(HandoffTransitionEvent::kDowngradeCompleted);
    return expect_true(step.transitioned && step.state == HandoffState::kPreLogin, "downgrade complete -> pre-login");
}

bool test_failure_and_reset() {
    using redclaw::session::HandoffState;
    using redclaw::session::HandoffTransitionEvent;
    using redclaw::session::SessionHandoffStateMachine;

    SessionHandoffStateMachine sm(HandoffState::kLaunchingHelper);
    auto step = sm.on_event(HandoffTransitionEvent::kHelperLaunchFailed);
    if (!expect_true(step.transitioned && step.state == HandoffState::kFailed, "launch failure -> failed")) {
        return false;
    }

    step = sm.on_event(HandoffTransitionEvent::kResetToPreLogin);
    return expect_true(step.transitioned && step.state == HandoffState::kPreLogin, "failed -> pre-login reset");
}

bool test_illegal_transition_rejected() {
    using redclaw::session::HandoffState;
    using redclaw::session::HandoffTransitionEvent;
    using redclaw::session::SessionHandoffStateMachine;

    SessionHandoffStateMachine sm(HandoffState::kPreLogin);
    const auto step = sm.on_event(HandoffTransitionEvent::kCapabilitySyncSucceeded);
    return expect_true(!step.transitioned && !step.error.empty() && sm.state() == HandoffState::kPreLogin,
        "illegal transition should be rejected");
}

bool test_handoff_audit_logging() {
    using redclaw::session::HandoffAuditEvent;
    using redclaw::session::HandoffState;
    using redclaw::session::HandoffTransitionEvent;
    using redclaw::session::SessionHandoffStateMachine;

    std::vector<HandoffAuditEvent> audits;
    std::uint64_t fake_now = 1'710'000'123ULL;

    SessionHandoffStateMachine sm(
        HandoffState::kPreLogin,
        "session-audit",
        [&](const HandoffAuditEvent& event) {
            audits.push_back(event);
        },
        [&]() {
            return fake_now++;
        });

    const auto first = sm.on_event(HandoffTransitionEvent::kInteractiveSessionDetected);
    if (!expect_true(first.transitioned, "audit test first transition should succeed")) {
        return false;
    }

    const auto invalid = sm.on_event(HandoffTransitionEvent::kUserLoggedOut);
    if (!expect_true(!invalid.transitioned, "audit test invalid transition should fail")) {
        return false;
    }

    if (!expect_true(audits.size() == 2, "audit sink should receive two events")) {
        return false;
    }

    const bool first_ok = expect_true(
        audits[0].session_id == "session-audit"
            && audits[0].transitioned
            && audits[0].from == HandoffState::kPreLogin
            && audits[0].to == HandoffState::kLaunchingHelper
            && audits[0].event == HandoffTransitionEvent::kInteractiveSessionDetected,
        "first audit event fields should match transition");

    const bool second_ok = expect_true(
        !audits[1].transitioned
            && audits[1].from == HandoffState::kLaunchingHelper
            && audits[1].to == HandoffState::kLaunchingHelper
            && audits[1].event == HandoffTransitionEvent::kUserLoggedOut
            && !audits[1].error.empty(),
        "second audit event should capture rejected transition");

    const auto formatted = redclaw::session::format_handoff_audit_log_line(audits[0]);
    const bool format_ok = expect_true(
        formatted.find("component=session.handoff") != std::string::npos
            && formatted.find("session_id=session-audit") != std::string::npos
            && formatted.find("event=interactive_session_detected") != std::string::npos
            && formatted.find("transitioned=true") != std::string::npos,
        "formatted handoff audit line should contain core fields");

    return first_ok && second_ok && format_ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_handoff_happy_path() && ok;
    ok = test_logout_downgrade_path() && ok;
    ok = test_helper_lost_path() && ok;
    ok = test_failure_and_reset() && ok;
    ok = test_illegal_transition_rejected() && ok;
    ok = test_handoff_audit_logging() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_session_handoff_state_machine_tests" << '\n';
    return 0;
}

