#include <cstdint>
#include <iostream>
#include <string>

#include "redclaw/input/input_module.h"
#include "redclaw/service/service_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_broker_capability_drives_input_gate() {
    std::uint64_t fake_now = 1'710'000'000ULL;
    redclaw::input::InputPolicyGate gate;
    gate.set_permission_policy(redclaw::input::InputPermissionPolicy::kFullControlSecureDesktop);
    gate.set_privileged_control_mode(redclaw::input::PrivilegedControlMode::kDisabled);

    redclaw::service::InMemoryPrivilegedControlBroker broker(
        [&fake_now]() { return fake_now; },
        {},
        {},
        [&gate](const redclaw::service::CapabilityChangeEvent& event) {
            if (event.after == redclaw::service::CapabilityLevel::kFullControl) {
                gate.set_privileged_control_mode(redclaw::input::PrivilegedControlMode::kEnabled);
            } else {
                gate.set_privileged_control_mode(redclaw::input::PrivilegedControlMode::kDisabled);
            }
        });

    if (!expect_true(
            !gate.can_inject(redclaw::input::InputTarget::kSecureDesktop),
            "secure desktop should be blocked before full-control grant")) {
        return false;
    }

    redclaw::service::PrivilegedRequest request;
    request.session_id = "session-integration";
    request.step_up_proof.operator_id = "operator-integration";
    request.step_up_proof.device_fingerprint = "device-integration";
    request.step_up_proof.challenge_id = "challenge-integration";
    request.step_up_proof.signed_proof = "proof";
    request.step_up_proof.issued_at_unix = fake_now;

    const auto granted = broker.requestPrivilegedControl(request);
    if (!expect_true(granted.accepted, "privileged request should be granted in integration test")) {
        return false;
    }

    if (!expect_true(
            gate.can_inject(redclaw::input::InputTarget::kSecureDesktop),
            "secure desktop should be allowed after full-control grant")) {
        return false;
    }

    const bool revoked = broker.revokePrivilegedControl("session-integration", "integration_test_revoke");
    if (!expect_true(revoked, "revoke should succeed")) {
        return false;
    }

    return expect_true(
        !gate.can_inject(redclaw::input::InputTarget::kSecureDesktop),
        "secure desktop should be blocked again after revoke");
}

}  // namespace

int main() {
    if (!test_broker_capability_drives_input_gate()) {
        return 1;
    }

    std::cout << "[PASS] redclaw_privileged_input_gate_integration_tests" << '\n';
    return 0;
}
