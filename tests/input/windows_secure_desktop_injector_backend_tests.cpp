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

bool test_channel_availability_gate() {
    redclaw::input::WindowsSecureDesktopInjectorBackend backend;

    redclaw::input::InputEvent event;
    event.target = redclaw::input::InputTarget::kSecureDesktop;
    event.type = redclaw::input::InputEventType::kMouseMove;
    event.x = 1;
    event.y = 1;

    const bool blocked_without_channel = !backend.inject(event);
    backend.set_channel_available(true);
    const bool open_channel_state = backend.channel_available();
    backend.set_channel_available(false);
    const bool closed_channel_state = !backend.channel_available();

    return expect_true(blocked_without_channel, "inject should be blocked when secure channel is unavailable")
        && expect_true(open_channel_state, "channel should report available after enable")
        && expect_true(closed_channel_state, "channel should report unavailable after disable");
}

bool test_forwarded_backend_failure_surface_with_invalid_mouse_button() {
    redclaw::input::WindowsSecureDesktopInjectorBackend backend;
    backend.set_channel_available(true);

    redclaw::input::InputEvent invalid_down_event;
    invalid_down_event.target = redclaw::input::InputTarget::kSecureDesktop;
    invalid_down_event.type = redclaw::input::InputEventType::kMouseButtonDown;
    invalid_down_event.mouse_button = redclaw::input::MouseButton::kNone;

    redclaw::input::InputEvent invalid_up_event;
    invalid_up_event.target = redclaw::input::InputTarget::kSecureDesktop;
    invalid_up_event.type = redclaw::input::InputEventType::kMouseButtonUp;
    invalid_up_event.mouse_button = redclaw::input::MouseButton::kNone;

    const bool down_rejected = !backend.inject(invalid_down_event);
    const bool up_rejected = !backend.inject(invalid_up_event);

    return expect_true(down_rejected, "invalid mouse-button-down should fail in delegated backend")
        && expect_true(up_rejected, "invalid mouse-button-up should fail in delegated backend")
        && expect_true(backend.channel_available(), "channel availability should remain true after delegated failures");
}

bool test_secure_desktop_probe_gates_backend_path() {
    redclaw::input::InMemoryInputInjectorBackend delegated_backend;
    redclaw::input::WindowsSecureDesktopInjectorBackend backend(
        []() { return false; },
        &delegated_backend);
    backend.set_channel_available(true);

    redclaw::input::InputEvent event;
    event.target = redclaw::input::InputTarget::kSecureDesktop;
    event.type = redclaw::input::InputEventType::kMouseMove;
    event.x = 11;
    event.y = 22;

    const bool blocked = !backend.inject(event);
    return expect_true(!backend.os_secure_desktop_ready(), "probe should report secure desktop unavailable")
        && expect_true(blocked, "inject should be fail-closed when OS secure desktop probe is false")
        && expect_true(delegated_backend.injected_events().empty(), "delegated backend should not be called when probe blocks");
}

bool test_secure_desktop_probe_allows_delegated_backend_when_ready() {
    redclaw::input::InMemoryInputInjectorBackend delegated_backend;
    redclaw::input::WindowsSecureDesktopInjectorBackend backend(
        []() { return true; },
        &delegated_backend);
    backend.set_channel_available(true);

    redclaw::input::InputEvent event;
    event.target = redclaw::input::InputTarget::kSecureDesktop;
    event.type = redclaw::input::InputEventType::kMouseMove;
    event.x = 33;
    event.y = 44;

    const bool injected = backend.inject(event);
    return expect_true(backend.os_secure_desktop_ready(), "probe should report secure desktop ready")
        && expect_true(injected, "inject should pass when channel and probe are both ready")
        && expect_true(delegated_backend.injected_events().size() == 1, "delegated backend should receive one event");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_channel_availability_gate() && ok;
    ok = test_forwarded_backend_failure_surface_with_invalid_mouse_button() && ok;
    ok = test_secure_desktop_probe_gates_backend_path() && ok;
    ok = test_secure_desktop_probe_allows_delegated_backend_when_ready() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_windows_secure_desktop_injector_backend_tests" << '\n';
    return 0;
}
