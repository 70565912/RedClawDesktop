#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace redclaw::input {

// Windows mouse delivery can truncate dwExtraInfo to 32 bits, including when
// both sender and receiver are 64-bit. Use one local marker for both event kinds.
inline constexpr std::uintptr_t kRedClawInputExtraInfo = 0x52434449UL;

enum class InputPermissionPolicy {
	kDenyAll,
	kStandardDesktopOnly,
	kFullControlSecureDesktop,
};

enum class PrivilegedControlMode {
	kDisabled,
	kEnabled,
};

enum class InputTarget {
	kUserDesktop,
	kSecureDesktop,
};

enum class InputEventType {
	kKeyDown,
	kKeyUp,
	kMouseMove,
	kMouseButtonDown,
	kMouseButtonUp,
	kMouseWheel,
	kMouseHorizontalWheel,
};

enum class MouseButton {
	kNone,
	kLeft,
	kRight,
	kMiddle,
	kX1,
	kX2,
};

struct InputEvent {
	InputTarget target = InputTarget::kUserDesktop;
	InputEventType type = InputEventType::kKeyDown;
	std::int32_t key_code = 0;
	std::uint16_t scan_code = 0;
	std::uint16_t virtual_key = 0;
	bool extended = false;
	bool repeat = false;
	std::int32_t x = 0;
	std::int32_t y = 0;
	bool absolute_coordinates = false;
	MouseButton mouse_button = MouseButton::kNone;
	std::int32_t wheel_delta = 0;
};

struct InjectResult {
	bool injected = false;
	std::string reason;
};

class InputPolicyGate {
public:
	void set_permission_policy(InputPermissionPolicy policy);
	void set_privileged_control_mode(PrivilegedControlMode mode);

	[[nodiscard]] bool can_inject(InputTarget target) const;
	[[nodiscard]] std::string deny_reason(InputTarget target) const;

private:
	InputPermissionPolicy policy_ = InputPermissionPolicy::kDenyAll;
	PrivilegedControlMode privileged_mode_ = PrivilegedControlMode::kDisabled;
};

class IInputInjectorBackend {
public:
	virtual ~IInputInjectorBackend() = default;
	virtual bool inject(const InputEvent& event) = 0;
	virtual bool inject_batch(const std::vector<InputEvent>& events);
};

class InMemoryInputInjectorBackend final : public IInputInjectorBackend {
public:
	bool inject(const InputEvent& event) override;
	bool inject_batch(const std::vector<InputEvent>& events) override;
	void set_fail_mode(bool fail_mode);

	[[nodiscard]] const std::vector<InputEvent>& injected_events() const;

private:
	bool fail_mode_ = false;
	std::vector<InputEvent> injected_events_;
};

class InputInjectionAdapter {
public:
	InputInjectionAdapter(InputPolicyGate& policy_gate, IInputInjectorBackend& backend);

	InjectResult inject_event(const InputEvent& event) const;
	InjectResult inject_events(const std::vector<InputEvent>& events) const;

	[[nodiscard]] static bool is_valid_event(const InputEvent& event);

private:
	InputPolicyGate& policy_gate_;
	IInputInjectorBackend& backend_;
};

class TargetRoutingInputInjectionAdapter {
public:
	TargetRoutingInputInjectionAdapter(
		InputPolicyGate& policy_gate,
		IInputInjectorBackend& user_desktop_backend,
		IInputInjectorBackend& secure_desktop_backend);

	InjectResult inject_event(const InputEvent& event) const;

private:
	InputPolicyGate& policy_gate_;
	IInputInjectorBackend& user_desktop_backend_;
	IInputInjectorBackend& secure_desktop_backend_;
};

class SecureDesktopBackendGate final : public IInputInjectorBackend {
public:
	explicit SecureDesktopBackendGate(IInputInjectorBackend& backend);

	void set_enabled(bool enabled);
	[[nodiscard]] bool enabled() const;

	bool inject(const InputEvent& event) override;

private:
	IInputInjectorBackend& backend_;
	bool enabled_ = false;
};

class CapabilityDrivenInputRuntimeController {
public:
	CapabilityDrivenInputRuntimeController(InputPolicyGate& policy_gate, SecureDesktopBackendGate& secure_backend_gate);

	void on_full_control_changed(bool enabled) const;

private:
	InputPolicyGate& policy_gate_;
	SecureDesktopBackendGate& secure_backend_gate_;
};

// Count-only opt-in evidence. Desktop: 0 unknown, 1 Default, 2 Winlogon, 3 other.
// Context is sampled on the actual injector thread, never inferred from an Agent.
struct SendInputDiagnostic {
    std::uint64_t begin_us = 0, end_us = 0;
    std::array<std::uint32_t, 7> counts{};
    std::uint32_t requested = 0, inserted = 0, error = 0;
    bool context_sampled = false, cursor_before_valid = false, cursor_after_valid = false;
    std::uint32_t process_id = 0, thread_id = 0, session_id = UINT32_MAX;
    std::uint32_t input_desktop = 0, thread_desktop = 0, desktop_error = 0;
    std::uint32_t foreground_pid = 0, foreground_session = UINT32_MAX, focus_pid = 0;
    std::uint32_t process_integrity = 0, foreground_integrity = 0;
    std::uint32_t process_integrity_error = 0, foreground_integrity_error = 0;
    std::int32_t cursor_before_x = 0, cursor_before_y = 0, cursor_after_x = 0, cursor_after_y = 0;
};

class WindowsSendInputInjectorBackend final : public IInputInjectorBackend {
public:
	bool inject(const InputEvent& event) override;
	bool inject_batch(const std::vector<InputEvent>& events) override;
    void set_diagnostic_observer(std::function<void(const SendInputDiagnostic&)> observer);
private:
    std::function<void(const SendInputDiagnostic&)> diagnostic_observer_;
    std::uint64_t last_context_us_ = 0;
};

struct DesktopGeometry {
	std::int32_t origin_x = 0;
	std::int32_t origin_y = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t rotation = 0;
	std::uint64_t revision = 0;
};

struct DesktopCaptureRegion {
	std::uint32_t x = 0;
	std::uint32_t y = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
};

struct DesktopPoint {
	std::int32_t x = 0;
	std::int32_t y = 0;
};

[[nodiscard]] bool is_valid_desktop_geometry(const DesktopGeometry& geometry);
[[nodiscard]] bool map_normalized_desktop_point(
	std::uint16_t normalized_x,
	std::uint16_t normalized_y,
	const DesktopGeometry& geometry,
	DesktopPoint* point);
[[nodiscard]] bool map_normalized_capture_region_point(
	std::uint16_t normalized_x,
	std::uint16_t normalized_y,
	const DesktopGeometry& geometry,
	const DesktopCaptureRegion& region,
	DesktopPoint* point);

enum class RemoteInputSessionState {
	kUnavailable,
	kAvailable,
	kActive,
	kPaused,
	kDenied,
};

enum class RemoteInputPauseReason {
	kNone,
	kNotAuthorized,
	kNoVideo,
	kLocalPause,
	kLeaseExpired,
	kQueueOverflow,
	kInjectionFailed,
	kDisconnected,
	kGeometryChanged,
	kStaleSequence,
};

struct RemoteInputSessionStats {
	std::uint64_t received_batches = 0;
	std::uint64_t injected_events = 0;
	std::uint64_t rejected_batches = 0;
	std::uint64_t release_all_count = 0;
	std::size_t queue_peak = 0;
	std::uint64_t last_applied_sequence = 0;
};

struct InputInjectionReceipt {
    std::uint64_t sequence = 0, begin_us = 0, end_us = 0;
    InputEventType type = InputEventType::kKeyDown;
    bool injected = false;
};

class RemoteInputSession final {
public:
	static constexpr std::size_t kQueueCapacity = 256;
	static constexpr std::uint64_t kLeaseDurationMs = 3000;

	RemoteInputSession(InputPolicyGate& policy_gate, IInputInjectorBackend& backend);
	~RemoteInputSession();

	void set_authorized(bool authorized);
    // A temporary transfer reason preserves independently applied capture,
    // consent, geometry and disconnect pauses. Entering releases all input.
    void set_transfer_blocked(bool blocked, std::uint64_t now_ms);
    [[nodiscard]] bool clipboard_paste_eligible() const;
    [[nodiscard]] std::uint64_t eligibility_revision() const { return eligibility_revision_; }
    // Trusted clipboard coordinator only: one fixed Ctrl+V during its transfer
    // gate. This neither opens general input nor drains queued remote events.
    [[nodiscard]] bool paste_verified_clipboard(std::uint64_t expected_revision, std::string* error = nullptr);
	[[nodiscard]] bool request_active(std::uint64_t now_ms, std::string* error = nullptr);
	[[nodiscard]] bool enqueue_batch(
		std::uint64_t sequence,
		std::vector<InputEvent> events,
		std::uint64_t now_ms,
		std::string* error = nullptr);
	[[nodiscard]] bool synchronize_state(
		std::uint64_t sequence,
		const std::vector<std::uint16_t>& pressed_scan_codes,
		std::uint32_t pressed_mouse_buttons,
		std::uint64_t now_ms,
		std::string* error = nullptr);
	[[nodiscard]] bool drain(std::uint64_t now_ms, std::string* error = nullptr);
	[[nodiscard]] bool expire_lease(std::uint64_t now_ms);
	void pause(RemoteInputPauseReason reason);
	void release_all();
    void set_injection_observer(std::function<void(const InputInjectionReceipt&)> observer);

	[[nodiscard]] bool authorized() const;
	[[nodiscard]] RemoteInputSessionState state() const;
	[[nodiscard]] RemoteInputPauseReason pause_reason() const;
	[[nodiscard]] std::size_t queued_event_count() const;
	[[nodiscard]] const RemoteInputSessionStats& stats() const;

private:
	[[nodiscard]] static std::uint32_t key_identity(const InputEvent& event);
	[[nodiscard]] static std::uint32_t button_mask(MouseButton button);
	[[nodiscard]] std::vector<InputEvent> build_release_events() const;
	void fail_closed(RemoteInputPauseReason reason);

	InputPolicyGate& policy_gate_;
	InputInjectionAdapter adapter_;
	bool authorized_ = false;
    bool transfer_blocked_ = false;
    std::uint64_t eligibility_revision_ = 0;
	RemoteInputSessionState state_ = RemoteInputSessionState::kDenied;
	RemoteInputPauseReason pause_reason_ = RemoteInputPauseReason::kNotAuthorized;
    struct QueuedEvent { InputEvent event; std::uint64_t sequence; };
	std::deque<QueuedEvent> queue_;
    std::function<void(const InputInjectionReceipt&)> injection_observer_;
	std::set<std::uint32_t> pressed_keys_;
	std::uint32_t pressed_mouse_buttons_ = 0;
	std::uint64_t last_received_sequence_ = 0;
	std::uint64_t lease_expires_at_ms_ = 0;
	RemoteInputSessionStats stats_;
};

class WindowsSecureDesktopInjectorBackend final : public IInputInjectorBackend {
public:
    using SecureDesktopActiveProbe = std::function<bool()>;

    explicit WindowsSecureDesktopInjectorBackend(
        SecureDesktopActiveProbe secure_desktop_probe = {},
        IInputInjectorBackend* delegated_backend = nullptr);

	void set_channel_available(bool available);
	[[nodiscard]] bool channel_available() const;
	[[nodiscard]] bool os_secure_desktop_ready() const;

	bool inject(const InputEvent& event) override;

private:
	bool channel_available_ = false;
	SecureDesktopActiveProbe secure_desktop_probe_;
	IInputInjectorBackend* delegated_backend_ = nullptr;
	WindowsSendInputInjectorBackend send_input_backend_;
};

std::string_view module_name();
}
