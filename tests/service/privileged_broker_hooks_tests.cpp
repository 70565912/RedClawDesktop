#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "redclaw/service/service_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_step_up_verifier_and_audit_sink() {
    std::uint64_t fake_now = 1'710'000'000ULL;
    std::vector<redclaw::service::PrivilegedAuditEvent> audits;
    std::vector<redclaw::service::CapabilityChangeEvent> capability_changes;

    redclaw::service::InMemoryPrivilegedControlBroker broker(
        [&fake_now]() { return fake_now; },
        [](const redclaw::service::PrivilegedRequest& request) {
            return request.step_up_proof.signed_proof == "approved-proof";
        },
        [&audits](const redclaw::service::PrivilegedAuditEvent& event) {
            audits.push_back(event);
        },
        [&capability_changes](const redclaw::service::CapabilityChangeEvent& event) {
            capability_changes.push_back(event);
        });

    redclaw::service::PrivilegedRequest denied;
    denied.session_id = "session-hooks";
    denied.step_up_proof.operator_id = "operator-hooks";
    denied.step_up_proof.device_fingerprint = "device-hooks";
    denied.step_up_proof.challenge_id = "challenge-denied";
    denied.step_up_proof.signed_proof = "wrong-proof";
    denied.step_up_proof.issued_at_unix = fake_now;

    const auto denied_result = broker.requestPrivilegedControl(denied);
    if (!expect_true(!denied_result.accepted, "custom verifier should deny non-approved proof")) {
        return false;
    }

    if (!expect_true(
            denied_result.error == redclaw::service::PrivilegedError::kStepUpRequired,
            "denied request should return step-up required")) {
        return false;
    }

    redclaw::service::PrivilegedRequest allowed = denied;
    allowed.step_up_proof.challenge_id = "challenge-allowed";
    allowed.step_up_proof.signed_proof = "approved-proof";

    const auto allowed_result = broker.requestPrivilegedControl(allowed);
    if (!expect_true(allowed_result.accepted, "approved proof should pass verifier")) {
        return false;
    }

    if (!expect_true(
            broker.beginUacPrompt("session-hooks", "uac-hooks-1"),
            "beginUacPrompt should succeed when full-control is active")) {
        return false;
    }

    if (!expect_true(
            broker.reportSecureDesktopUnavailable("session-hooks", "secure_desktop_transport_lost"),
            "secure desktop unavailable should revoke full control")) {
        return false;
    }

    if (!expect_true(!audits.empty(), "audit sink should receive events")) {
        return false;
    }

    bool saw_denied = false;
    bool saw_granted = false;
    for (const auto& event : audits) {
        if (event.detail == "step_up_failed") {
            saw_denied = true;
        }
        if (event.detail == "granted") {
            saw_granted = true;
        }
    }

    const bool capability_granted =
        capability_changes.size() >= 1
        && capability_changes[0].before == redclaw::service::CapabilityLevel::kStandardControl
        && capability_changes[0].after == redclaw::service::CapabilityLevel::kFullControl;

    const bool capability_revoked_by_secure_desktop =
        capability_changes.size() >= 2
        && capability_changes[1].before == redclaw::service::CapabilityLevel::kFullControl
        && capability_changes[1].after == redclaw::service::CapabilityLevel::kStandardControl;

    if (!expect_true(capability_revoked_by_secure_desktop, "capability hook should report secure-desktop revoke")) {
        return false;
    }

    redclaw::service::PrivilegedRequest regrant = allowed;
    regrant.step_up_proof.challenge_id = "challenge-regrant";
    const auto regrant_result = broker.requestPrivilegedControl(regrant);
    if (!expect_true(regrant_result.accepted, "regrant should succeed after secure-desktop revoke")) {
        return false;
    }

    const bool revoked = broker.revokePrivilegedControl("session-hooks", "test_revoke");
    const bool capability_revoked =
        revoked
        && capability_changes.size() >= 4
        && capability_changes[3].before == redclaw::service::CapabilityLevel::kFullControl
        && capability_changes[3].after == redclaw::service::CapabilityLevel::kStandardControl;

    bool saw_uac_prompt_raised = false;
    bool saw_secure_desktop_unavailable = false;
    for (const auto& event : audits) {
        if (event.action == "uac_prompt_state" && event.detail == "raised") {
            saw_uac_prompt_raised = true;
        }
        if (event.action == "uac_prompt_state" && event.error == redclaw::service::PrivilegedError::kSecureDesktopUnavailable) {
            saw_secure_desktop_unavailable = true;
        }
    }

    return expect_true(saw_denied, "audit log should contain denied event")
        && expect_true(saw_granted, "audit log should contain granted event")
        && expect_true(saw_uac_prompt_raised, "audit log should contain uac prompt raised event")
        && expect_true(saw_secure_desktop_unavailable, "audit log should contain secure desktop unavailable event")
        && expect_true(capability_granted, "capability hook should report grant")
        && expect_true(capability_revoked, "capability hook should report revoke");
}

bool test_privileged_audit_log_format_redacts_sensitive_fields() {
    redclaw::service::PrivilegedAuditEvent event;
    event.session_id = "session-hooks";
    event.operator_id = "operator-hooks";
    event.action = "request_privileged";
    event.detail = "token_signature_mismatch";
    event.decision = redclaw::service::PrivilegedDecision::kBlocked;
    event.error = redclaw::service::PrivilegedError::kReplayDetected;
    event.timestamp_unix = 1'710'000'001ULL;

    const auto line = redclaw::service::format_privileged_audit_log_line(event);

    return expect_true(
               line.find("component=service.privileged_broker") != std::string::npos,
               "formatted audit line should include service component")
        && expect_true(
            line.find("event=request_privileged") != std::string::npos,
            "formatted audit line should include action as event")
        && expect_true(
            line.find("session_id=session-hooks") != std::string::npos,
            "session_id should remain visible")
        && expect_true(
            line.find("operator_id=operator-hooks") != std::string::npos,
            "operator_id should remain visible")
        && expect_true(
            line.find("auth_detail=[REDACTED:") != std::string::npos,
            "sensitive detail value should be redacted")
        && expect_true(
            line.find("error=replay_detected") != std::string::npos,
            "line should include error code field")
        && expect_true(
            line.find("decision=blocked") != std::string::npos,
            "line should include decision field");
}

}  // namespace

int main() {
    if (!test_step_up_verifier_and_audit_sink()) {
        return 1;
    }

    if (!test_privileged_audit_log_format_redacts_sensitive_fields()) {
        return 1;
    }

    std::cout << "[PASS] redclaw_service_privileged_broker_hooks_tests" << '\n';
    return 0;
}
