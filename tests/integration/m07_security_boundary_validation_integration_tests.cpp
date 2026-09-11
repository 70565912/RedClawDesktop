#include <iostream>
#include <string>

#include "redclaw/service/helper_launcher.h"
#include "redclaw/service/ipc_channel_contracts.h"
#include "redclaw/service/service_module.h"
#include "redclaw/session/session_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }
    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

redclaw::service::HelperLaunchRequest base_request(std::string session_id, std::uint32_t user_session_id) {
    redclaw::service::HelperLaunchRequest req;
    req.session_id = std::move(session_id);
    req.user_session_id = user_session_id;
    req.helper_executable_path = "C:\\Program Files\\RedClaw\\redclaw_user_session_helper.exe";
    req.service_pipe_name = "\\\\.\\pipe\\redclaw_service_ipc";
    return req;
}

bool test_ipc_handshake_auth_and_schema_validation() {
    using namespace redclaw::service;

    IpcHandshakeRequest req;
    req.header.schema_version = kIpcSchemaVersionV1;
    req.header.message_type = IpcMessageType::kHandshakeRequest;
    req.service_auth_token = "valid-token";
    req.session_id = "session-security";
    req.helper_process_id = "helper-1";

    std::string error;
    if (!expect_true(validate_ipc_handshake_request(req, &error), "baseline handshake request should validate")) {
        return false;
    }

    req.service_auth_token.clear();
    if (!expect_true(!validate_ipc_handshake_request(req, &error), "missing auth token should be rejected")) {
        return false;
    }

    req.service_auth_token = "valid-token";
    req.header.schema_version = 999;
    const auto parsed = parse_ipc_handshake_request(serialize_ipc_handshake_request(req));
    return expect_true(!parsed.ok, "unexpected schema version should be rejected during parse");
}

bool test_ipc_message_boundary_validation() {
    using namespace redclaw::service;

    IpcCapabilityAdvertisement ad;
    ad.header.schema_version = kIpcSchemaVersionV1;
    ad.header.message_type = IpcMessageType::kCapabilityAdvertisement;
    ad.session_id.clear();

    std::string error;
    if (!expect_true(!validate_ipc_capability_advertisement(ad, &error), "capability ad without session_id should fail")) {
        return false;
    }

    const auto malformed = parse_ipc_handshake_request("WRONG-HEADER-V1\nschema_version=1\n");
    return expect_true(!malformed.ok, "malformed IPC header should be rejected");
}

bool test_helper_launcher_session_isolation() {
    using namespace redclaw::service;

    HelperProcessLauncher launcher;
    const auto launched = launcher.launch_helper(base_request("session-a", 101));
    if (!expect_true(launched.launched, "helper launch for session-a should succeed")) {
        return false;
    }

    const auto duplicate = launcher.launch_helper(base_request("session-a", 101));
    if (!expect_true(!duplicate.launched && duplicate.error == HelperLaunchError::kAlreadyRunning,
            "duplicate launch in same session should be rejected")) {
        return false;
    }

    std::string stop_error;
    if (!expect_true(!launcher.stop_helper("session-b", &stop_error), "cannot stop helper from another session id")) {
        return false;
    }

    const auto invalid = launcher.launch_helper(base_request("session-c", 0));
    if (!expect_true(!invalid.launched && invalid.error == HelperLaunchError::kInvalidRequest,
            "invalid user session id should be rejected")) {
        return false;
    }

    return expect_true(launcher.stop_helper("session-a", &stop_error), "cleanup stop should succeed");
}

bool test_privileged_boundary_denies_unverified_stepup() {
    using namespace redclaw::service;

    InMemoryPrivilegedControlBroker broker(
        []() { return static_cast<std::uint64_t>(1'710'000'500ULL); },
        [](const PrivilegedRequest& req) { return req.step_up_proof.signed_proof == "approved-proof"; });

    PrivilegedRequest req;
    req.session_id = "security-session";
    req.reason_code = "request_full_control";
    req.step_up_proof.operator_id = "operator-x";
    req.step_up_proof.device_fingerprint = "device-x";
    req.step_up_proof.challenge_id = "challenge-1";
    req.step_up_proof.signed_proof = "invalid-proof";
    req.step_up_proof.issued_at_unix = 1'710'000'500ULL;

    const auto result = broker.requestPrivilegedControl(req);
    return expect_true(
        !result.accepted
            && result.error == PrivilegedError::kStepUpRequired
            && broker.currentCapability("security-session") == CapabilityLevel::kStandardControl,
        "unverified step-up proof should be denied and capability should remain standard");
}

bool test_handoff_illegal_transition_blocked() {
    using redclaw::session::HandoffState;
    using redclaw::session::HandoffTransitionEvent;
    using redclaw::session::SessionHandoffStateMachine;

    SessionHandoffStateMachine sm(HandoffState::kPreLogin);
    const auto update = sm.on_event(HandoffTransitionEvent::kCapabilitySyncSucceeded);
    return expect_true(
        !update.transitioned && sm.state() == HandoffState::kPreLogin && !update.error.empty(),
        "illegal handoff transition should be blocked");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_ipc_handshake_auth_and_schema_validation() && ok;
    ok = test_ipc_message_boundary_validation() && ok;
    ok = test_helper_launcher_session_isolation() && ok;
    ok = test_privileged_boundary_denies_unverified_stepup() && ok;
    ok = test_handoff_illegal_transition_blocked() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_m07_security_boundary_validation_integration_tests" << '\n';
    return 0;
}

