#include "redclaw/input/input_module.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "windows_input_diagnostics.h"
#endif

namespace redclaw::input {

namespace {

bool default_secure_desktop_active_probe() {
#if !defined(_WIN32)
    return false;
#else
    HDESK desktop = OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (desktop == nullptr) {
        return false;
    }

    char desktop_name[256] = {};
    DWORD bytes_needed = 0;
    const bool ok = GetUserObjectInformationA(
                        desktop,
                        UOI_NAME,
                        desktop_name,
                        static_cast<DWORD>(sizeof(desktop_name)),
                        &bytes_needed)
        != FALSE;
    CloseDesktop(desktop);

    if (!ok) {
        return false;
    }

    // UAC consent and pre-login flows run on Winlogon desktop.
    return lstrcmpiA(desktop_name, "Winlogon") == 0;
#endif
}

}  // namespace

bool IInputInjectorBackend::inject_batch(const std::vector<InputEvent>& events) {
    for (const auto& event : events) {
        if (!inject(event)) {
            return false;
        }
    }
    return true;
}

void InputPolicyGate::set_permission_policy(InputPermissionPolicy policy) {
    policy_ = policy;
}

void InputPolicyGate::set_privileged_control_mode(PrivilegedControlMode mode) {
    privileged_mode_ = mode;
}

bool InputPolicyGate::can_inject(InputTarget target) const {
    switch (policy_) {
        case InputPermissionPolicy::kDenyAll:
            return false;
        case InputPermissionPolicy::kStandardDesktopOnly:
            return target == InputTarget::kUserDesktop;
        case InputPermissionPolicy::kFullControlSecureDesktop:
            if (target == InputTarget::kUserDesktop) {
                return true;
            }
            return privileged_mode_ == PrivilegedControlMode::kEnabled;
    }

    return false;
}

std::string InputPolicyGate::deny_reason(InputTarget target) const {
    if (can_inject(target)) {
        return {};
    }

    if (policy_ == InputPermissionPolicy::kDenyAll) {
        return "policy_denies_all_input";
    }

    if (target == InputTarget::kSecureDesktop && privileged_mode_ != PrivilegedControlMode::kEnabled) {
        return "secure_desktop_requires_privileged_mode";
    }

    return "target_denied_by_policy";
}

bool InMemoryInputInjectorBackend::inject(const InputEvent& event) {
    if (fail_mode_) {
        return false;
    }

    injected_events_.push_back(event);
    return true;
}

bool InMemoryInputInjectorBackend::inject_batch(const std::vector<InputEvent>& events) {
    if (fail_mode_) {
        return false;
    }
    injected_events_.insert(injected_events_.end(), events.begin(), events.end());
    return true;
}

void InMemoryInputInjectorBackend::set_fail_mode(bool fail_mode) {
    fail_mode_ = fail_mode;
}

const std::vector<InputEvent>& InMemoryInputInjectorBackend::injected_events() const {
    return injected_events_;
}

InputInjectionAdapter::InputInjectionAdapter(InputPolicyGate& policy_gate, IInputInjectorBackend& backend)
    : policy_gate_(policy_gate), backend_(backend) {}

bool InputInjectionAdapter::is_valid_event(const InputEvent& event) {
    switch (event.type) {
        case InputEventType::kKeyDown:
        case InputEventType::kKeyUp:
            return event.key_code > 0 || event.scan_code > 0;
        case InputEventType::kMouseMove:
            return true;
        case InputEventType::kMouseButtonDown:
        case InputEventType::kMouseButtonUp:
            return event.mouse_button != MouseButton::kNone;
        case InputEventType::kMouseWheel:
        case InputEventType::kMouseHorizontalWheel:
            return event.wheel_delta != 0;
    }

    return false;
}

InjectResult InputInjectionAdapter::inject_events(const std::vector<InputEvent>& events) const {
    InjectResult result;
    if (events.empty()) {
        result.injected = true;
        return result;
    }
    for (const auto& event : events) {
        if (!is_valid_event(event)) {
            result.reason = "invalid_input_event";
            return result;
        }
        if (!policy_gate_.can_inject(event.target)) {
            result.reason = policy_gate_.deny_reason(event.target);
            return result;
        }
    }
    if (!backend_.inject_batch(events)) {
        result.reason = "backend_injection_failed";
        return result;
    }
    result.injected = true;
    return result;
}

InjectResult InputInjectionAdapter::inject_event(const InputEvent& event) const {
    InjectResult result;
    if (!is_valid_event(event)) {
        result.reason = "invalid_input_event";
        return result;
    }

    if (!policy_gate_.can_inject(event.target)) {
        result.reason = policy_gate_.deny_reason(event.target);
        return result;
    }

    if (!backend_.inject(event)) {
        result.reason = "backend_injection_failed";
        return result;
    }

    result.injected = true;
    return result;
}

TargetRoutingInputInjectionAdapter::TargetRoutingInputInjectionAdapter(
    InputPolicyGate& policy_gate,
    IInputInjectorBackend& user_desktop_backend,
    IInputInjectorBackend& secure_desktop_backend)
    : policy_gate_(policy_gate),
      user_desktop_backend_(user_desktop_backend),
      secure_desktop_backend_(secure_desktop_backend) {}

InjectResult TargetRoutingInputInjectionAdapter::inject_event(const InputEvent& event) const {
    InjectResult result;
    if (!InputInjectionAdapter::is_valid_event(event)) {
        result.reason = "invalid_input_event";
        return result;
    }

    if (!policy_gate_.can_inject(event.target)) {
        result.reason = policy_gate_.deny_reason(event.target);
        return result;
    }

    IInputInjectorBackend* backend = &user_desktop_backend_;
    if (event.target == InputTarget::kSecureDesktop) {
        backend = &secure_desktop_backend_;
    }

    if (!backend->inject(event)) {
        result.reason = "backend_injection_failed";
        return result;
    }

    result.injected = true;
    return result;
}

SecureDesktopBackendGate::SecureDesktopBackendGate(IInputInjectorBackend& backend)
    : backend_(backend) {}

void SecureDesktopBackendGate::set_enabled(bool enabled) {
    enabled_ = enabled;
}

bool SecureDesktopBackendGate::enabled() const {
    return enabled_;
}

bool SecureDesktopBackendGate::inject(const InputEvent& event) {
    if (!enabled_) {
        return false;
    }

    return backend_.inject(event);
}

CapabilityDrivenInputRuntimeController::CapabilityDrivenInputRuntimeController(
    InputPolicyGate& policy_gate,
    SecureDesktopBackendGate& secure_backend_gate)
    : policy_gate_(policy_gate), secure_backend_gate_(secure_backend_gate) {}

void CapabilityDrivenInputRuntimeController::on_full_control_changed(bool enabled) const {
    policy_gate_.set_privileged_control_mode(
        enabled ? PrivilegedControlMode::kEnabled : PrivilegedControlMode::kDisabled);
    secure_backend_gate_.set_enabled(enabled);
}

bool WindowsSendInputInjectorBackend::inject(const InputEvent& event) {
    return inject_batch(std::vector<InputEvent>{event});
}

void WindowsSendInputInjectorBackend::set_diagnostic_observer(
    std::function<void(const SendInputDiagnostic&)> observer) {
    diagnostic_observer_ = std::move(observer);
    last_context_us_ = 0;
}

bool WindowsSendInputInjectorBackend::inject_batch(const std::vector<InputEvent>& events) {
#if !defined(_WIN32)
    (void)events;
    return false;
#else
    if (events.empty()) {
        return true;
    }
    std::vector<INPUT> inputs;
    inputs.reserve(events.size());
    for (const auto& event : events) {
        INPUT input{};
        switch (event.type) {
        case InputEventType::kKeyDown:
        case InputEventType::kKeyUp:
            input.type = INPUT_KEYBOARD;
            input.ki.dwExtraInfo = static_cast<ULONG_PTR>(kRedClawInputExtraInfo);
            if (event.scan_code > 0) {
                input.ki.wScan = event.scan_code;
                input.ki.dwFlags = KEYEVENTF_SCANCODE;
                if (event.extended) {
                    input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
                }
            } else {
                input.ki.wVk = static_cast<WORD>(event.key_code);
            }
            if (event.type == InputEventType::kKeyUp) {
                input.ki.dwFlags |= KEYEVENTF_KEYUP;
            }
            break;
        case InputEventType::kMouseMove:
            input.type = INPUT_MOUSE;
            input.mi.dwExtraInfo = static_cast<ULONG_PTR>(kRedClawInputExtraInfo);
            if (event.absolute_coordinates) {
                const int virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                const int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                if (virtual_width <= 0 || virtual_height <= 0) {
                    return false;
                }
                const auto normalize = [](std::int32_t value, int origin, int extent) {
                    if (extent <= 1) {
                        return 0L;
                    }
                    const double scaled = static_cast<double>(value - origin) * 65535.0
                        / static_cast<double>(extent - 1);
                    return static_cast<LONG>(std::clamp(std::lround(scaled), 0L, 65535L));
                };
                input.mi.dx = normalize(event.x, virtual_x, virtual_width);
                input.mi.dy = normalize(event.y, virtual_y, virtual_height);
                input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            } else {
                input.mi.dx = static_cast<LONG>(event.x);
                input.mi.dy = static_cast<LONG>(event.y);
                input.mi.dwFlags = MOUSEEVENTF_MOVE;
            }
            break;
        case InputEventType::kMouseButtonDown:
        case InputEventType::kMouseButtonUp:
            input.type = INPUT_MOUSE;
            input.mi.dwExtraInfo = static_cast<ULONG_PTR>(kRedClawInputExtraInfo);
            switch (event.mouse_button) {
                case MouseButton::kLeft:
                    input.mi.dwFlags = event.type == InputEventType::kMouseButtonDown
                        ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                    break;
                case MouseButton::kRight:
                    input.mi.dwFlags = event.type == InputEventType::kMouseButtonDown
                        ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                    break;
                case MouseButton::kMiddle:
                    input.mi.dwFlags = event.type == InputEventType::kMouseButtonDown
                        ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
                    break;
                case MouseButton::kX1:
                    input.mi.dwFlags = event.type == InputEventType::kMouseButtonDown
                        ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
                    input.mi.mouseData = XBUTTON1;
                    break;
                case MouseButton::kX2:
                    input.mi.dwFlags = event.type == InputEventType::kMouseButtonDown
                        ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
                    input.mi.mouseData = XBUTTON2;
                    break;
                case MouseButton::kNone:
                    return false;
            }
            break;
        case InputEventType::kMouseWheel:
        case InputEventType::kMouseHorizontalWheel:
            input.type = INPUT_MOUSE;
            input.mi.dwExtraInfo = static_cast<ULONG_PTR>(kRedClawInputExtraInfo);
            input.mi.dwFlags = event.type == InputEventType::kMouseWheel
                ? MOUSEEVENTF_WHEEL : MOUSEEVENTF_HWHEEL;
            input.mi.mouseData = static_cast<DWORD>(event.wheel_delta);
            break;
        }
        inputs.push_back(input);
    }
    SendInputDiagnostic diagnostic;
    const auto clock_us = [] { return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()); };
    if (diagnostic_observer_) {
        const auto now = clock_us();
        if (!last_context_us_ || now - last_context_us_ >= 250000) {
            detail::sample_context(diagnostic); last_context_us_ = now;
        }
        for (const auto& event : events) {
            const auto kind = static_cast<std::size_t>(event.type);
            if (kind < diagnostic.counts.size()) ++diagnostic.counts[kind];
        }
        diagnostic.requested = static_cast<std::uint32_t>(inputs.size());
        diagnostic.begin_us = clock_us();
        SetLastError(ERROR_SUCCESS);
    }
    const UINT inserted = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    if (diagnostic_observer_) {
        diagnostic.error = inserted == inputs.size() ? 0U : GetLastError();
        diagnostic.end_us = clock_us(); diagnostic.inserted = inserted;
        if (diagnostic.context_sampled) {
            POINT cursor{}; diagnostic.cursor_after_valid = GetCursorPos(&cursor) != FALSE;
            diagnostic.cursor_after_x = cursor.x; diagnostic.cursor_after_y = cursor.y;
        }
        diagnostic_observer_(diagnostic);
    }
    if (inserted == inputs.size()) {
        return true;
    }

    // SendInput can report a partial batch. Conservatively release every key/button
    // that this batch attempted to press so an untracked partial insert cannot leave
    // the Host desktop with a stuck modifier or mouse button.
    std::vector<INPUT> compensating_releases;
    compensating_releases.reserve(events.size());
    for (const auto& event : events) {
        if (event.type != InputEventType::kKeyDown
            && event.type != InputEventType::kMouseButtonDown) {
            continue;
        }
        INPUT release{};
        if (event.type == InputEventType::kKeyDown) {
            release.type = INPUT_KEYBOARD;
            release.ki.dwExtraInfo = static_cast<ULONG_PTR>(kRedClawInputExtraInfo);
            if (event.scan_code > 0) {
                release.ki.wScan = event.scan_code;
                release.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
                if (event.extended) {
                    release.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
                }
            } else {
                release.ki.wVk = static_cast<WORD>(event.key_code);
                release.ki.dwFlags = KEYEVENTF_KEYUP;
            }
        } else {
            release.type = INPUT_MOUSE;
            release.mi.dwExtraInfo = static_cast<ULONG_PTR>(kRedClawInputExtraInfo);
            switch (event.mouse_button) {
            case MouseButton::kLeft: release.mi.dwFlags = MOUSEEVENTF_LEFTUP; break;
            case MouseButton::kRight: release.mi.dwFlags = MOUSEEVENTF_RIGHTUP; break;
            case MouseButton::kMiddle: release.mi.dwFlags = MOUSEEVENTF_MIDDLEUP; break;
            case MouseButton::kX1:
                release.mi.dwFlags = MOUSEEVENTF_XUP;
                release.mi.mouseData = XBUTTON1;
                break;
            case MouseButton::kX2:
                release.mi.dwFlags = MOUSEEVENTF_XUP;
                release.mi.mouseData = XBUTTON2;
                break;
            case MouseButton::kNone:
                continue;
            }
        }
        compensating_releases.push_back(release);
    }
    if (!compensating_releases.empty()) {
        (void)SendInput(
            static_cast<UINT>(compensating_releases.size()),
            compensating_releases.data(),
            sizeof(INPUT));
    }
    return false;
#endif
}

bool is_valid_desktop_geometry(const DesktopGeometry& geometry) {
    return geometry.width > 0 && geometry.height > 0 && geometry.revision > 0
        && geometry.rotation <= 270 && geometry.rotation % 90 == 0;
}

bool map_normalized_desktop_point(
    std::uint16_t normalized_x,
    std::uint16_t normalized_y,
    const DesktopGeometry& geometry,
    DesktopPoint* point) {
    if (point == nullptr || !is_valid_desktop_geometry(geometry)) {
        return false;
    }
    const double x_ratio = static_cast<double>(normalized_x) / 65535.0;
    const double y_ratio = static_cast<double>(normalized_y) / 65535.0;
    double mapped_x = x_ratio;
    double mapped_y = y_ratio;
    switch (geometry.rotation) {
    case 0:
        break;
    case 90:
        mapped_x = y_ratio;
        mapped_y = 1.0 - x_ratio;
        break;
    case 180:
        mapped_x = 1.0 - x_ratio;
        mapped_y = 1.0 - y_ratio;
        break;
    case 270:
        mapped_x = 1.0 - y_ratio;
        mapped_y = x_ratio;
        break;
    default:
        return false;
    }
    point->x = geometry.origin_x + static_cast<std::int32_t>(
        std::lround(mapped_x * static_cast<double>(geometry.width - 1)));
    point->y = geometry.origin_y + static_cast<std::int32_t>(
        std::lround(mapped_y * static_cast<double>(geometry.height - 1)));
    return true;
}

bool map_normalized_capture_region_point(
    std::uint16_t normalized_x,
    std::uint16_t normalized_y,
    const DesktopGeometry& geometry,
    const DesktopCaptureRegion& region,
    DesktopPoint* point) {
    if (!is_valid_desktop_geometry(geometry) || point == nullptr
        || region.width == 0 || region.height == 0
        || region.x + region.width > geometry.width
        || region.y + region.height > geometry.height) {
        return false;
    }
    const auto map_axis = [](std::uint16_t value,
                             std::uint32_t offset,
                             std::uint32_t extent,
                             std::uint32_t full_extent) {
        const std::uint64_t pixel = static_cast<std::uint64_t>(offset)
            + (static_cast<std::uint64_t>(value) * (extent - 1U) + 32767ULL)
                / 65535ULL;
        if (full_extent <= 1) {
            return static_cast<std::uint16_t>(0);
        }
        return static_cast<std::uint16_t>((std::min<std::uint64_t>)(
            65535ULL,
            (pixel * 65535ULL + (full_extent - 1U) / 2U)
                / (full_extent - 1U)));
    };
    return map_normalized_desktop_point(
        map_axis(normalized_x, region.x, region.width, geometry.width),
        map_axis(normalized_y, region.y, region.height, geometry.height),
        geometry,
        point);
}

RemoteInputSession::RemoteInputSession(
    InputPolicyGate& policy_gate,
    IInputInjectorBackend& backend)
    : policy_gate_(policy_gate), adapter_(policy_gate, backend) {
    policy_gate_.set_permission_policy(InputPermissionPolicy::kDenyAll);
}

RemoteInputSession::~RemoteInputSession() {
    release_all();
}

void RemoteInputSession::set_authorized(bool authorized) {
    if (!authorized) {
        release_all();
        queue_.clear();
        authorized_ = false;
        state_ = RemoteInputSessionState::kDenied;
        pause_reason_ = RemoteInputPauseReason::kNotAuthorized;
        policy_gate_.set_permission_policy(InputPermissionPolicy::kDenyAll);
        return;
    }
    authorized_ = true;
    policy_gate_.set_permission_policy(InputPermissionPolicy::kStandardDesktopOnly);
    state_ = RemoteInputSessionState::kAvailable;
    pause_reason_ = RemoteInputPauseReason::kNone;
}

bool RemoteInputSession::request_active(std::uint64_t now_ms, std::string* error) {
    if (!authorized_) {
        if (error != nullptr) {
            *error = "remote input is not authorized";
        }
        return false;
    }
    release_all();
    queue_.clear();
    state_ = RemoteInputSessionState::kActive;
    pause_reason_ = RemoteInputPauseReason::kNone;
    lease_expires_at_ms_ = now_ms + kLeaseDurationMs;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool RemoteInputSession::enqueue_batch(
    std::uint64_t sequence,
    std::vector<InputEvent> events,
    std::uint64_t now_ms,
    std::string* error) {
    if (state_ != RemoteInputSessionState::kActive || !authorized_) {
        ++stats_.rejected_batches;
        if (error != nullptr) {
            *error = "remote input session is not active";
        }
        return false;
    }
    if (sequence <= last_received_sequence_) {
        ++stats_.rejected_batches;
        pause_reason_ = RemoteInputPauseReason::kStaleSequence;
        if (error != nullptr) {
            *error = "remote input sequence is stale";
        }
        return false;
    }
    if (events.empty() || events.size() > kQueueCapacity
        || queue_.size() + events.size() > kQueueCapacity) {
        ++stats_.rejected_batches;
        fail_closed(RemoteInputPauseReason::kQueueOverflow);
        if (error != nullptr) {
            *error = "remote input queue overflow";
        }
        return false;
    }
    last_received_sequence_ = sequence;
    ++stats_.received_batches;
    for (auto& event : events) {
        queue_.push_back({std::move(event), sequence});
    }
    stats_.queue_peak = (std::max)(stats_.queue_peak, queue_.size());
    lease_expires_at_ms_ = now_ms + kLeaseDurationMs;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool RemoteInputSession::synchronize_state(
    std::uint64_t sequence,
    const std::vector<std::uint16_t>& pressed_scan_codes,
    std::uint32_t pressed_mouse_buttons,
    std::uint64_t now_ms,
    std::string* error) {
    if (state_ != RemoteInputSessionState::kActive || !authorized_) {
        if (error != nullptr) {
            *error = "remote input session is not active";
        }
        return false;
    }
    if (sequence <= last_received_sequence_) {
        if (error != nullptr) {
            *error = "remote input state sequence is stale";
        }
        return false;
    }
    if (!drain(now_ms, error)) {
        return false;
    }
    last_received_sequence_ = sequence;
    std::set<std::uint32_t> remote_keys;
    for (const std::uint16_t encoded : pressed_scan_codes) {
        const std::uint16_t scan_code = encoded & 0x7FFFU;
        if (scan_code != 0) {
            remote_keys.insert(static_cast<std::uint32_t>(scan_code)
                | ((encoded & 0x8000U) != 0 ? 0x10000U : 0U));
        }
    }
    std::vector<InputEvent> releases;
    for (const std::uint32_t key : pressed_keys_) {
        if (!remote_keys.contains(key)) {
            InputEvent event;
            event.type = InputEventType::kKeyUp;
            event.scan_code = static_cast<std::uint16_t>(key & 0xFFFFU);
            event.extended = (key & 0x10000U) != 0;
            releases.push_back(event);
        }
    }
    for (const auto button : {MouseButton::kLeft, MouseButton::kRight, MouseButton::kMiddle,
                              MouseButton::kX1, MouseButton::kX2}) {
        const std::uint32_t mask = button_mask(button);
        if ((pressed_mouse_buttons_ & mask) != 0 && (pressed_mouse_buttons & mask) == 0) {
            InputEvent event;
            event.type = InputEventType::kMouseButtonUp;
            event.mouse_button = button;
            releases.push_back(event);
        }
    }
    if (!releases.empty()) {
        const auto result = adapter_.inject_events(releases);
        if (!result.injected) {
            fail_closed(RemoteInputPauseReason::kInjectionFailed);
            if (error != nullptr) {
                *error = result.reason;
            }
            return false;
        }
    }
    for (auto iter = pressed_keys_.begin(); iter != pressed_keys_.end();) {
        if (!remote_keys.contains(*iter)) {
            iter = pressed_keys_.erase(iter);
        } else {
            ++iter;
        }
    }
    pressed_mouse_buttons_ &= pressed_mouse_buttons;
    lease_expires_at_ms_ = now_ms + kLeaseDurationMs;
    stats_.last_applied_sequence = sequence;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool RemoteInputSession::drain(std::uint64_t now_ms, std::string* error) {
    if (state_ != RemoteInputSessionState::kActive) {
        return true;
    }
    if (expire_lease(now_ms)) {
        if (error != nullptr) {
            *error = "remote input lease expired";
        }
        return false;
    }
    if (queue_.empty()) {
        return true;
    }
    std::vector<InputEvent> events;
    events.reserve(queue_.size());
    std::vector<InputInjectionReceipt> receipts;
    if (injection_observer_) receipts.reserve(queue_.size());
    std::set<std::uint32_t> next_keys = pressed_keys_;
    std::uint32_t next_buttons = pressed_mouse_buttons_;
    while (!queue_.empty()) {
        auto queued = std::move(queue_.front());
        InputEvent event = std::move(queued.event);
        queue_.pop_front();
        if (event.type == InputEventType::kKeyDown) {
            const std::uint32_t key = key_identity(event);
            if (next_keys.contains(key) && !event.repeat) {
                continue;
            }
            next_keys.insert(key);
        } else if (event.type == InputEventType::kKeyUp) {
            const std::uint32_t key = key_identity(event);
            if (!next_keys.contains(key)) {
                continue;
            }
            next_keys.erase(key);
        } else if (event.type == InputEventType::kMouseButtonDown) {
            const std::uint32_t mask = button_mask(event.mouse_button);
            if ((next_buttons & mask) != 0) {
                continue;
            }
            next_buttons |= mask;
        } else if (event.type == InputEventType::kMouseButtonUp) {
            const std::uint32_t mask = button_mask(event.mouse_button);
            if ((next_buttons & mask) == 0) {
                continue;
            }
            next_buttons &= ~mask;
        }
        if (injection_observer_) receipts.push_back({.sequence = queued.sequence, .type = event.type});
        events.push_back(std::move(event));
    }
    const auto monotonic_us = [] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    const auto injection_begin_us = injection_observer_ ? monotonic_us() : 0;
    const auto result = adapter_.inject_events(events);
    if (injection_observer_) {
        const auto end_us = monotonic_us();
        for (auto& receipt : receipts) {
            receipt.begin_us = injection_begin_us; receipt.end_us = end_us; receipt.injected = result.injected;
            injection_observer_(receipt);
        }
    }
    if (!result.injected) {
        fail_closed(RemoteInputPauseReason::kInjectionFailed);
        if (error != nullptr) {
            *error = result.reason;
        }
        return false;
    }
    pressed_keys_ = std::move(next_keys);
    pressed_mouse_buttons_ = next_buttons;
    stats_.injected_events += events.size();
    stats_.last_applied_sequence = last_received_sequence_;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool RemoteInputSession::expire_lease(std::uint64_t now_ms) {
    if (state_ != RemoteInputSessionState::kActive || lease_expires_at_ms_ == 0
        || now_ms < lease_expires_at_ms_) {
        return false;
    }
    fail_closed(RemoteInputPauseReason::kLeaseExpired);
    return true;
}

void RemoteInputSession::set_injection_observer(std::function<void(const InputInjectionReceipt&)> observer) {
    injection_observer_ = std::move(observer);
}

void RemoteInputSession::pause(RemoteInputPauseReason reason) {
    release_all();
    queue_.clear();
    lease_expires_at_ms_ = 0;
    if (reason == RemoteInputPauseReason::kDisconnected) {
        last_received_sequence_ = 0;
    }
    state_ = authorized_ ? RemoteInputSessionState::kPaused : RemoteInputSessionState::kDenied;
    pause_reason_ = authorized_ ? reason : RemoteInputPauseReason::kNotAuthorized;
}

void RemoteInputSession::release_all() {
    const std::vector<InputEvent> releases = build_release_events();
    if (!releases.empty()) {
        (void)adapter_.inject_events(releases);
    }
    pressed_keys_.clear();
    pressed_mouse_buttons_ = 0;
    ++stats_.release_all_count;
}

bool RemoteInputSession::authorized() const { return authorized_; }
RemoteInputSessionState RemoteInputSession::state() const { return state_; }
RemoteInputPauseReason RemoteInputSession::pause_reason() const { return pause_reason_; }
std::size_t RemoteInputSession::queued_event_count() const { return queue_.size(); }
const RemoteInputSessionStats& RemoteInputSession::stats() const { return stats_; }

std::uint32_t RemoteInputSession::key_identity(const InputEvent& event) {
    return static_cast<std::uint32_t>(event.scan_code)
        | (event.extended ? 0x10000U : 0U);
}

std::uint32_t RemoteInputSession::button_mask(MouseButton button) {
    switch (button) {
    case MouseButton::kLeft: return 1U << 0U;
    case MouseButton::kRight: return 1U << 1U;
    case MouseButton::kMiddle: return 1U << 2U;
    case MouseButton::kX1: return 1U << 3U;
    case MouseButton::kX2: return 1U << 4U;
    case MouseButton::kNone: return 0;
    }
    return 0;
}

std::vector<InputEvent> RemoteInputSession::build_release_events() const {
    std::vector<InputEvent> releases;
    releases.reserve(pressed_keys_.size() + 5);
    for (const std::uint32_t key : pressed_keys_) {
        InputEvent event;
        event.type = InputEventType::kKeyUp;
        event.scan_code = static_cast<std::uint16_t>(key & 0xFFFFU);
        event.extended = (key & 0x10000U) != 0;
        releases.push_back(event);
    }
    for (const auto button : {MouseButton::kLeft, MouseButton::kRight, MouseButton::kMiddle,
                              MouseButton::kX1, MouseButton::kX2}) {
        if ((pressed_mouse_buttons_ & button_mask(button)) != 0) {
            InputEvent event;
            event.type = InputEventType::kMouseButtonUp;
            event.mouse_button = button;
            releases.push_back(event);
        }
    }
    return releases;
}

void RemoteInputSession::fail_closed(RemoteInputPauseReason reason) {
    release_all();
    queue_.clear();
    lease_expires_at_ms_ = 0;
    state_ = authorized_ ? RemoteInputSessionState::kPaused : RemoteInputSessionState::kDenied;
    pause_reason_ = reason;
}

WindowsSecureDesktopInjectorBackend::WindowsSecureDesktopInjectorBackend(
    SecureDesktopActiveProbe secure_desktop_probe,
    IInputInjectorBackend* delegated_backend)
    : secure_desktop_probe_(std::move(secure_desktop_probe)) {
    if (!secure_desktop_probe_) {
        secure_desktop_probe_ = default_secure_desktop_active_probe;
    }

    delegated_backend_ = delegated_backend != nullptr
        ? delegated_backend
        : static_cast<IInputInjectorBackend*>(&send_input_backend_);
}

void WindowsSecureDesktopInjectorBackend::set_channel_available(bool available) {
    channel_available_ = available;
}

bool WindowsSecureDesktopInjectorBackend::channel_available() const {
    return channel_available_;
}

bool WindowsSecureDesktopInjectorBackend::os_secure_desktop_ready() const {
    return secure_desktop_probe_ != nullptr && secure_desktop_probe_();
}

bool WindowsSecureDesktopInjectorBackend::inject(const InputEvent& event) {
    if (!channel_available_ || !os_secure_desktop_ready()) {
        return false;
    }

    return delegated_backend_->inject(event);
}

std::string_view module_name() {
    return "input";
}
}
