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

bool test_policy_deny_blocks_backend_call() {
    redclaw::input::InputPolicyGate gate;
    gate.set_permission_policy(redclaw::input::InputPermissionPolicy::kDenyAll);

    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::InputInjectionAdapter adapter(gate, backend);

    redclaw::input::InputEvent event;
    event.target = redclaw::input::InputTarget::kUserDesktop;
    event.type = redclaw::input::InputEventType::kKeyDown;
    event.key_code = 65;

    const auto result = adapter.inject_event(event);
    return expect_true(!result.injected, "deny-all policy should block injection")
        && expect_true(result.reason == "policy_denies_all_input", "deny reason should come from policy gate")
        && expect_true(backend.injected_events().empty(), "backend should not receive blocked events");
}

bool test_allowed_event_reaches_backend() {
    redclaw::input::InputPolicyGate gate;
    gate.set_permission_policy(redclaw::input::InputPermissionPolicy::kStandardDesktopOnly);

    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::InputInjectionAdapter adapter(gate, backend);

    redclaw::input::InputEvent event;
    event.target = redclaw::input::InputTarget::kUserDesktop;
    event.type = redclaw::input::InputEventType::kMouseMove;
    event.x = 640;
    event.y = 360;

    const auto result = adapter.inject_event(event);
    return expect_true(result.injected, "allowed event should be injected")
        && expect_true(result.reason.empty(), "allowed event should have empty reason")
        && expect_true(backend.injected_events().size() == 1, "backend should record one injected event");
}

bool test_secure_desktop_requires_privileged_mode() {
    redclaw::input::InputPolicyGate gate;
    gate.set_permission_policy(redclaw::input::InputPermissionPolicy::kFullControlSecureDesktop);
    gate.set_privileged_control_mode(redclaw::input::PrivilegedControlMode::kDisabled);

    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::InputInjectionAdapter adapter(gate, backend);

    redclaw::input::InputEvent event;
    event.target = redclaw::input::InputTarget::kSecureDesktop;
    event.type = redclaw::input::InputEventType::kMouseButtonDown;
    event.mouse_button = redclaw::input::MouseButton::kLeft;

    const auto blocked = adapter.inject_event(event);
    if (!expect_true(!blocked.injected, "secure desktop should be blocked without privileged mode")) {
        return false;
    }

    if (!expect_true(
            blocked.reason == "secure_desktop_requires_privileged_mode",
            "secure desktop block reason should match")) {
        return false;
    }

    gate.set_privileged_control_mode(redclaw::input::PrivilegedControlMode::kEnabled);
    const auto allowed = adapter.inject_event(event);

    return expect_true(allowed.injected, "secure desktop should inject when privileged mode is enabled")
        && expect_true(backend.injected_events().size() == 1, "backend should receive secure desktop event when enabled");
}

bool test_backend_failure_propagates_error() {
    redclaw::input::InputPolicyGate gate;
    gate.set_permission_policy(redclaw::input::InputPermissionPolicy::kStandardDesktopOnly);

    redclaw::input::InMemoryInputInjectorBackend backend;
    backend.set_fail_mode(true);
    redclaw::input::InputInjectionAdapter adapter(gate, backend);

    redclaw::input::InputEvent event;
    event.target = redclaw::input::InputTarget::kUserDesktop;
    event.type = redclaw::input::InputEventType::kKeyUp;
    event.key_code = 65;

    const auto result = adapter.inject_event(event);
    return expect_true(!result.injected, "backend failure should fail injection")
        && expect_true(result.reason == "backend_injection_failed", "backend failure reason should be propagated")
        && expect_true(backend.injected_events().empty(), "failed backend should not record events");
}

bool test_invalid_event_rejected_before_policy_and_backend() {
    redclaw::input::InputPolicyGate gate;
    gate.set_permission_policy(redclaw::input::InputPermissionPolicy::kStandardDesktopOnly);

    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::InputInjectionAdapter adapter(gate, backend);

    redclaw::input::InputEvent invalid_key_event;
    invalid_key_event.target = redclaw::input::InputTarget::kUserDesktop;
    invalid_key_event.type = redclaw::input::InputEventType::kKeyDown;
    invalid_key_event.key_code = 0;

    const auto invalid_key = adapter.inject_event(invalid_key_event);
    if (!expect_true(!invalid_key.injected, "invalid key event should be rejected")) {
        return false;
    }

    if (!expect_true(invalid_key.reason == "invalid_input_event", "invalid key event reason should match")) {
        return false;
    }

    redclaw::input::InputEvent invalid_button_event;
    invalid_button_event.target = redclaw::input::InputTarget::kUserDesktop;
    invalid_button_event.type = redclaw::input::InputEventType::kMouseButtonDown;
    invalid_button_event.mouse_button = redclaw::input::MouseButton::kNone;

    const auto invalid_button = adapter.inject_event(invalid_button_event);
    if (!expect_true(!invalid_button.injected, "invalid mouse button event should be rejected")) {
        return false;
    }

    if (!expect_true(invalid_button.reason == "invalid_input_event", "invalid button event reason should match")) {
        return false;
    }

    redclaw::input::InputEvent invalid_wheel_event;
    invalid_wheel_event.target = redclaw::input::InputTarget::kUserDesktop;
    invalid_wheel_event.type = redclaw::input::InputEventType::kMouseWheel;
    invalid_wheel_event.wheel_delta = 0;

    const auto invalid_wheel = adapter.inject_event(invalid_wheel_event);
    return expect_true(!invalid_wheel.injected, "invalid wheel event should be rejected")
        && expect_true(invalid_wheel.reason == "invalid_input_event", "invalid wheel event reason should match")
        && expect_true(backend.injected_events().empty(), "backend should not receive invalid events");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_policy_deny_blocks_backend_call() && ok;
    ok = test_allowed_event_reaches_backend() && ok;
    ok = test_secure_desktop_requires_privileged_mode() && ok;
    ok = test_backend_failure_propagates_error() && ok;
    ok = test_invalid_event_rejected_before_policy_and_backend() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_input_injection_adapter_tests" << '\n';
    return 0;
}