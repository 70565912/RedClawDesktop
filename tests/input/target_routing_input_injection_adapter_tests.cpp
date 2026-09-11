#include <iostream>
#include <string>

#include "redclaw/input/input_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_routes_user_desktop_and_secure_desktop_to_different_backends() {
    redclaw::input::InputPolicyGate gate;
    gate.set_permission_policy(redclaw::input::InputPermissionPolicy::kFullControlSecureDesktop);
    gate.set_privileged_control_mode(redclaw::input::PrivilegedControlMode::kEnabled);

    redclaw::input::InMemoryInputInjectorBackend user_backend;
    redclaw::input::InMemoryInputInjectorBackend secure_backend;

    redclaw::input::TargetRoutingInputInjectionAdapter adapter(gate, user_backend, secure_backend);

    redclaw::input::InputEvent user_event;
    user_event.target = redclaw::input::InputTarget::kUserDesktop;
    user_event.type = redclaw::input::InputEventType::kMouseMove;
    user_event.x = 100;
    user_event.y = 200;

    redclaw::input::InputEvent secure_event;
    secure_event.target = redclaw::input::InputTarget::kSecureDesktop;
    secure_event.type = redclaw::input::InputEventType::kMouseButtonDown;
    secure_event.mouse_button = redclaw::input::MouseButton::kLeft;

    const auto user_result = adapter.inject_event(user_event);
    const bool user_ok = user_result.injected
        && user_backend.injected_events().size() == 1
        && secure_backend.injected_events().empty();

    if (!expect_true(user_ok, "user desktop event should route only to user backend")) {
        return false;
    }

    const auto secure_result = adapter.inject_event(secure_event);

    const bool secure_ok = secure_result.injected
        && user_backend.injected_events().size() == 1
        && secure_backend.injected_events().size() == 1;

    return expect_true(secure_ok, "secure desktop event should route only to secure backend");
}

bool test_secure_backend_failure_propagates_error() {
    redclaw::input::InputPolicyGate gate;
    gate.set_permission_policy(redclaw::input::InputPermissionPolicy::kFullControlSecureDesktop);
    gate.set_privileged_control_mode(redclaw::input::PrivilegedControlMode::kEnabled);

    redclaw::input::InMemoryInputInjectorBackend user_backend;
    redclaw::input::InMemoryInputInjectorBackend secure_backend;
    secure_backend.set_fail_mode(true);

    redclaw::input::TargetRoutingInputInjectionAdapter adapter(gate, user_backend, secure_backend);

    redclaw::input::InputEvent secure_event;
    secure_event.target = redclaw::input::InputTarget::kSecureDesktop;
    secure_event.type = redclaw::input::InputEventType::kMouseButtonUp;
    secure_event.mouse_button = redclaw::input::MouseButton::kLeft;

    const auto result = adapter.inject_event(secure_event);
    return expect_true(!result.injected, "secure backend failure should fail injection")
        && expect_true(result.reason == "backend_injection_failed", "secure backend failure reason should propagate")
        && expect_true(user_backend.injected_events().empty(), "user backend should remain untouched")
        && expect_true(secure_backend.injected_events().empty(), "failed secure backend should not store event");
}

bool test_secure_desktop_policy_block_still_applies() {
    redclaw::input::InputPolicyGate gate;
    gate.set_permission_policy(redclaw::input::InputPermissionPolicy::kFullControlSecureDesktop);
    gate.set_privileged_control_mode(redclaw::input::PrivilegedControlMode::kDisabled);

    redclaw::input::InMemoryInputInjectorBackend user_backend;
    redclaw::input::InMemoryInputInjectorBackend secure_backend;

    redclaw::input::TargetRoutingInputInjectionAdapter adapter(gate, user_backend, secure_backend);

    redclaw::input::InputEvent secure_event;
    secure_event.target = redclaw::input::InputTarget::kSecureDesktop;
    secure_event.type = redclaw::input::InputEventType::kMouseButtonDown;
    secure_event.mouse_button = redclaw::input::MouseButton::kLeft;

    const auto result = adapter.inject_event(secure_event);
    return expect_true(!result.injected, "secure desktop should be blocked without privileged mode")
        && expect_true(
            result.reason == "secure_desktop_requires_privileged_mode",
            "policy deny reason should be preserved")
        && expect_true(user_backend.injected_events().empty(), "blocked event should not hit user backend")
        && expect_true(secure_backend.injected_events().empty(), "blocked event should not hit secure backend");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_routes_user_desktop_and_secure_desktop_to_different_backends() && ok;
    ok = test_secure_backend_failure_propagates_error() && ok;
    ok = test_secure_desktop_policy_block_still_applies() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_target_routing_input_injection_adapter_tests" << '\n';
    return 0;
}