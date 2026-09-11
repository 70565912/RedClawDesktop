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

bool test_broker_capability_controls_policy_and_secure_backend_gate() {
    std::uint64_t fake_now = 1'710'000'100ULL;

    redclaw::input::InputPolicyGate gate;
    gate.set_permission_policy(redclaw::input::InputPermissionPolicy::kFullControlSecureDesktop);
    gate.set_privileged_control_mode(redclaw::input::PrivilegedControlMode::kDisabled);

    redclaw::input::InMemoryInputInjectorBackend user_backend;
    redclaw::input::InMemoryInputInjectorBackend secure_backend_raw;
    redclaw::input::SecureDesktopBackendGate secure_backend_gate(secure_backend_raw);

    redclaw::input::TargetRoutingInputInjectionAdapter adapter(gate, user_backend, secure_backend_gate);
    redclaw::input::CapabilityDrivenInputRuntimeController runtime_controller(gate, secure_backend_gate);

    redclaw::service::InMemoryPrivilegedControlBroker broker(
        [&fake_now]() { return fake_now; },
        {},
        {},
        [&runtime_controller](const redclaw::service::CapabilityChangeEvent& event) {
            runtime_controller.on_full_control_changed(event.after == redclaw::service::CapabilityLevel::kFullControl);
        });

    redclaw::input::InputEvent secure_event;
    secure_event.target = redclaw::input::InputTarget::kSecureDesktop;
    secure_event.type = redclaw::input::InputEventType::kMouseButtonDown;
    secure_event.mouse_button = redclaw::input::MouseButton::kLeft;

    const auto before_grant = adapter.inject_event(secure_event);
    if (!expect_true(!before_grant.injected, "secure desktop should be blocked before full-control grant")) {
        return false;
    }

    if (!expect_true(
            before_grant.reason == "secure_desktop_requires_privileged_mode",
            "pre-grant deny reason should match policy")) {
        return false;
    }

    redclaw::service::PrivilegedRequest request;
    request.session_id = "session-runtime";
    request.step_up_proof.operator_id = "operator-runtime";
    request.step_up_proof.device_fingerprint = "device-runtime";
    request.step_up_proof.challenge_id = "challenge-runtime";
    request.step_up_proof.signed_proof = "proof";
    request.step_up_proof.issued_at_unix = fake_now;

    const auto grant = broker.requestPrivilegedControl(request);
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    const auto after_grant = adapter.inject_event(secure_event);
    if (!expect_true(after_grant.injected, "secure desktop should be injected after full-control grant")) {
        return false;
    }

    if (!expect_true(secure_backend_gate.enabled(), "secure backend gate should be enabled after grant")) {
        return false;
    }

    const bool revoked = broker.revokePrivilegedControl("session-runtime", "runtime_integration_revoke");
    if (!expect_true(revoked, "revoke should succeed")) {
        return false;
    }

    const auto after_revoke = adapter.inject_event(secure_event);
    return expect_true(!after_revoke.injected, "secure desktop should be blocked after revoke")
        && expect_true(
            after_revoke.reason == "secure_desktop_requires_privileged_mode",
            "post-revoke deny reason should match policy")
        && expect_true(!secure_backend_gate.enabled(), "secure backend gate should be disabled after revoke")
        && expect_true(secure_backend_raw.injected_events().size() == 1, "secure backend should only receive granted event");
}

}  // namespace

int main() {
    if (!test_broker_capability_controls_policy_and_secure_backend_gate()) {
        return 1;
    }

    std::cout << "[PASS] redclaw_privileged_runtime_input_routing_integration_tests" << '\n';
    return 0;
}