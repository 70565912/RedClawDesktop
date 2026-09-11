#include <array>
#include <iostream>
#include <string>

#include "redclaw/protocol/protocol_module.h"
#include "redclaw/session/session_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_happy_path_transitions() {
    using redclaw::protocol::SessionStateV1;
    using redclaw::session::SessionTransitionEvent;
    using redclaw::session::is_valid_session_transition;

    return expect_true(
               is_valid_session_transition(SessionStateV1::idle, SessionTransitionEvent::kOfferSent, SessionStateV1::offering),
               "idle -> offering via offer should be legal")
        && expect_true(
            is_valid_session_transition(SessionStateV1::offering, SessionTransitionEvent::kHandshakeValidated, SessionStateV1::connecting),
            "offering -> connecting via validated handshake should be legal")
        && expect_true(
            is_valid_session_transition(SessionStateV1::connecting, SessionTransitionEvent::kTransportConnected, SessionStateV1::established),
            "connecting -> established via transport connected should be legal");
}

bool test_recovery_success_transition() {
    using redclaw::protocol::SessionStateV1;
    using redclaw::session::SessionTransitionEvent;
    using redclaw::session::is_valid_session_transition;

    return expect_true(
               is_valid_session_transition(SessionStateV1::established, SessionTransitionEvent::kTransportDisconnected, SessionStateV1::recovering),
               "established -> recovering via disconnect should be legal")
        && expect_true(
            is_valid_session_transition(SessionStateV1::recovering, SessionTransitionEvent::kRecoverySucceeded, SessionStateV1::established),
            "recovering -> established via recovery success should be legal");
}

bool test_recovery_failure_transition() {
    using redclaw::protocol::SessionStateV1;
    using redclaw::session::SessionTransitionEvent;
    using redclaw::session::is_valid_session_transition;

    return expect_true(
               is_valid_session_transition(SessionStateV1::recovering, SessionTransitionEvent::kRecoveryFailed, SessionStateV1::terminated),
               "recovering -> terminated via recovery failure should be legal")
        && expect_true(
            is_valid_session_transition(SessionStateV1::recovering, SessionTransitionEvent::kTerminateRequested, SessionStateV1::terminated),
            "recovering -> terminated via explicit terminate should be legal");
}

bool test_illegal_transition_rejection() {
    using redclaw::protocol::SessionStateV1;
    using redclaw::session::SessionTransitionEvent;
    using redclaw::session::is_valid_session_transition;

    return expect_true(
               !is_valid_session_transition(SessionStateV1::idle, SessionTransitionEvent::kTransportConnected, SessionStateV1::established),
               "idle should not jump directly to established")
        && expect_true(
            !is_valid_session_transition(SessionStateV1::offering, SessionTransitionEvent::kTransportConnected, SessionStateV1::established),
            "offering should not jump directly to established")
        && expect_true(
            !is_valid_session_transition(SessionStateV1::established, SessionTransitionEvent::kOfferSent, SessionStateV1::offering),
            "established should not transition back to offering")
        && expect_true(
            !is_valid_session_transition(SessionStateV1::connecting, SessionTransitionEvent::kRecoverySucceeded, SessionStateV1::established),
            "connecting should not accept recovery-success event");
}

bool test_terminated_is_terminal_state() {
    using redclaw::protocol::SessionStateV1;
    using redclaw::session::SessionTransitionEvent;
    using redclaw::session::is_valid_session_transition;

    constexpr std::array<SessionTransitionEvent, 7> kEvents = {
        SessionTransitionEvent::kOfferSent,
        SessionTransitionEvent::kHandshakeValidated,
        SessionTransitionEvent::kTransportConnected,
        SessionTransitionEvent::kTransportDisconnected,
        SessionTransitionEvent::kRecoverySucceeded,
        SessionTransitionEvent::kRecoveryFailed,
        SessionTransitionEvent::kTerminateRequested,
    };

    bool ok = true;
    for (const auto event : kEvents) {
        ok = expect_true(
                 !is_valid_session_transition(SessionStateV1::terminated, event, SessionStateV1::idle),
                 "terminated should reject every outgoing transition")
            && ok;
    }

    return ok;
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_happy_path_transitions() && ok;
    ok = test_recovery_success_transition() && ok;
    ok = test_recovery_failure_transition() && ok;
    ok = test_illegal_transition_rejection() && ok;
    ok = test_terminated_is_terminal_state() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_session_state_transition_matrix_tests" << '\n';
    return 0;
}
