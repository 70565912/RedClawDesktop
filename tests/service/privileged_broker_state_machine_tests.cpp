#include <cstdint>
#include <iostream>
#include <string>

#include "redclaw/service/service_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_valid_transitions() {
    using redclaw::service::PrivilegedState;
    using redclaw::service::PrivilegedTransitionEvent;

    bool ok = true;
    ok = expect_true(
             redclaw::service::is_valid_transition(
                 PrivilegedState::kIdle,
                 PrivilegedTransitionEvent::kSessionAuthenticated,
                 PrivilegedState::kStandardActive),
             "idle -> standard should be valid")
        && ok;

    ok = expect_true(
             redclaw::service::is_valid_transition(
                 PrivilegedState::kStandardActive,
                 PrivilegedTransitionEvent::kRequestPrivilegedControl,
                 PrivilegedState::kPrivilegePending),
             "standard -> pending should be valid")
        && ok;

    ok = expect_true(
             redclaw::service::is_valid_transition(
                 PrivilegedState::kPrivilegePending,
                 PrivilegedTransitionEvent::kVerificationPassed,
                 PrivilegedState::kFullControlActive),
             "pending -> full control should be valid")
        && ok;

    ok = expect_true(
             redclaw::service::is_valid_transition(
                 PrivilegedState::kFullControlActive,
                 PrivilegedTransitionEvent::kUacPromptRaised,
                 PrivilegedState::kUacPromptActive),
             "full control -> uac prompt should be valid")
        && ok;

    return ok;
}

bool test_invalid_transition_rejected() {
    using redclaw::service::PrivilegedState;
    using redclaw::service::PrivilegedTransitionEvent;

    return expect_true(
        !redclaw::service::is_valid_transition(
            PrivilegedState::kIdle,
            PrivilegedTransitionEvent::kUacPromptRaised,
            PrivilegedState::kUacPromptActive),
        "idle should not transition directly to uac prompt");
}

bool test_replay_and_token_expiry_paths() {
    std::uint64_t fake_now = 1'710'000'000ULL;
    redclaw::service::InMemoryPrivilegedControlBroker broker([&fake_now]() { return fake_now; });

    redclaw::service::PrivilegedRequest first_request;
    first_request.session_id = "session-a";
    first_request.step_up_proof.operator_id = "operator-a";
    first_request.step_up_proof.device_fingerprint = "device-a";
    first_request.step_up_proof.challenge_id = "challenge-1";
    first_request.step_up_proof.signed_proof = "proof";
    first_request.step_up_proof.issued_at_unix = fake_now;

    const auto first_result = broker.requestPrivilegedControl(first_request);
    if (!expect_true(first_result.accepted, "first privileged request should be accepted")) {
        return false;
    }

    const std::string token_id = first_result.grant_token.token_id;

    redclaw::service::UacConsentAction prompt_not_raised_action;
    prompt_not_raised_action.session_id = "session-a";
    prompt_not_raised_action.token_id = token_id;
    prompt_not_raised_action.decision = redclaw::service::PrivilegedDecision::kAllow;
    prompt_not_raised_action.uac_prompt_id = "uac-001";

    const auto prompt_not_raised = broker.confirmUacConsent(prompt_not_raised_action);
    if (!expect_true(!prompt_not_raised.applied, "uac confirm should fail when prompt is not raised")) {
        return false;
    }

    if (!expect_true(
            prompt_not_raised.error == redclaw::service::PrivilegedError::kSecureDesktopUnavailable,
            "uac confirm without raised prompt should return secure desktop unavailable")) {
        return false;
    }

    if (!expect_true(broker.beginUacPrompt("session-a", "uac-001"), "beginUacPrompt should succeed after grant")) {
        return false;
    }

    redclaw::service::UacConsentAction mismatched_prompt_action;
    mismatched_prompt_action.session_id = "session-a";
    mismatched_prompt_action.token_id = token_id;
    mismatched_prompt_action.decision = redclaw::service::PrivilegedDecision::kAllow;
    mismatched_prompt_action.uac_prompt_id = "uac-wrong";

    const auto mismatched_prompt = broker.confirmUacConsent(mismatched_prompt_action);
    if (!expect_true(!mismatched_prompt.applied, "mismatched prompt id should fail")) {
        return false;
    }

    if (!expect_true(
            mismatched_prompt.error == redclaw::service::PrivilegedError::kSecureDesktopUnavailable,
            "mismatched prompt id should return secure desktop unavailable")) {
        return false;
    }

    // Replay the challenge id and ensure it is rejected.
    const auto replay_result = broker.requestPrivilegedControl(first_request);
    if (!expect_true(!replay_result.accepted, "replayed challenge should not be accepted")) {
        return false;
    }

    if (!expect_true(
            replay_result.error == redclaw::service::PrivilegedError::kReplayDetected,
            "replayed challenge should return replay error")) {
        return false;
    }

    if (!expect_true(
            broker.currentCapability("session-a") == redclaw::service::CapabilityLevel::kFullControl,
            "challenge replay should not downgrade existing full-control capability")) {
        return false;
    }

    if (!expect_true(
            broker.reportSecureDesktopUnavailable("session-a", "secure_desktop_transport_lost"),
            "secure desktop unavailable should revoke full control")) {
        return false;
    }

    if (!expect_true(
            broker.currentCapability("session-a") == redclaw::service::CapabilityLevel::kStandardControl,
            "secure desktop unavailable should downgrade capability to standard")) {
        return false;
    }

    first_request.step_up_proof.challenge_id = "challenge-2";
    const auto second_result = broker.requestPrivilegedControl(first_request);
    if (!expect_true(second_result.accepted, "fresh challenge should allow re-grant after secure desktop revoke")) {
        return false;
    }

    if (!expect_true(broker.beginUacPrompt("session-a", "uac-002"), "beginUacPrompt should succeed after re-grant")) {
        return false;
    }

    // Advance time beyond token TTL and ensure UAC confirm fails closed.
    fake_now += 121;

    redclaw::service::UacConsentAction action;
    action.session_id = "session-a";
    action.token_id = second_result.grant_token.token_id;
    action.decision = redclaw::service::PrivilegedDecision::kAllow;
    action.uac_prompt_id = "uac-002";

    const auto expired = broker.confirmUacConsent(action);
    return expect_true(!expired.applied, "expired token must not apply UAC action")
        && expect_true(
            expired.error == redclaw::service::PrivilegedError::kTokenExpired,
            "expired token should return token expired error")
        && expect_true(
            broker.currentCapability("session-a") == redclaw::service::CapabilityLevel::kStandardControl,
            "expired token should downgrade capability to standard");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_valid_transitions() && ok;
    ok = test_invalid_transition_rejected() && ok;
    ok = test_replay_and_token_expiry_paths() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_service_privileged_broker_state_machine_tests" << '\n';
    return 0;
}
