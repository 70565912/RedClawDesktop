#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "redclaw/input/input_module.h"
#include "redclaw/protocol/protocol_module.h"
#include "redclaw/security/security_module.h"
#include "redclaw/service/service_module.h"

namespace redclaw::session {

enum class DesktopSourceActivityState {
    kUnknown,
    kActive,
    kStaticPending,
    kStatic,
};

struct DesktopSourceActivityConfig {
    // Cursor blink / sparse desktop updates are still an active source.
    std::uint64_t static_quiet_ms = 2000;
    std::uint64_t capture_stall_ms = 1000;
    std::uint32_t minimum_timeout_polls = 3;
};

struct DesktopSourceActivitySnapshot {
    DesktopSourceActivityState state = DesktopSourceActivityState::kUnknown;
    std::uint64_t revision = 0;
    std::uint64_t reference_frame_id = 0;
    std::uint64_t reference_keyframe_id = 0;
    std::uint64_t stream_geometry_revision = 0;
    std::uint64_t rate_revision = 0;
    std::uint64_t capture_poll_started_total = 0;
    std::uint64_t capture_poll_completed_total = 0;
    std::uint64_t capture_timeout_total = 0;
    std::uint32_t consecutive_timeouts = 0;
    std::uint64_t last_poll_completed_ms = 0;
    std::uint64_t last_frame_ms = 0;
    std::uint64_t static_entry_total = 0;
    std::uint64_t static_exit_total = 0;
    std::uint64_t capture_stall_total = 0;
    std::uint64_t reference_retry_total = 0;
};

struct DesktopSourceActivityUpdate {
    bool state_changed = false;
    bool request_keyframe = false;
    bool request_reference_frame = false;
    DesktopSourceActivitySnapshot snapshot {};
};

class DesktopSourceActivityTracker {
public:
    explicit DesktopSourceActivityTracker(DesktopSourceActivityConfig config = {});

    void reset(std::uint64_t stream_geometry_revision, std::uint64_t rate_revision);
    void on_capture_poll_started(std::uint64_t now_ms);
    DesktopSourceActivityUpdate on_capture_timeout(std::uint64_t now_ms);
    DesktopSourceActivityUpdate on_capture_frame(
        std::uint64_t now_ms,
        std::uint64_t stream_geometry_revision,
        std::uint64_t rate_revision);
    DesktopSourceActivityUpdate on_capture_failure(std::uint64_t now_ms);
    DesktopSourceActivityUpdate tick(std::uint64_t now_ms);
    DesktopSourceActivityUpdate on_viewport_change(
        std::uint64_t now_ms,
        std::uint64_t stream_geometry_revision,
        std::uint64_t rate_revision);
    DesktopSourceActivityUpdate on_reference_submitted(
        std::uint64_t frame_id,
        std::uint64_t keyframe_id,
        std::uint64_t now_ms,
        std::uint64_t rate_revision);
    DesktopSourceActivityUpdate on_displayable_ack(
        std::uint64_t source_activity_revision,
        std::uint64_t latest_displayable_keyframe_id);
    [[nodiscard]] bool reference_retry_due(std::uint64_t now_ms, std::uint32_t srtt_ms) const;
    DesktopSourceActivityUpdate mark_reference_retry(std::uint64_t now_ms);
    [[nodiscard]] DesktopSourceActivitySnapshot snapshot() const;

private:
    DesktopSourceActivityUpdate update(bool changed, bool request_keyframe, bool request_reference) const;
    DesktopSourceActivityUpdate enter_unknown(bool stalled);
    DesktopSourceActivityUpdate enter_static_pending(bool request_keyframe);
    [[nodiscard]] std::uint64_t reference_retry_delay_ms(std::uint32_t srtt_ms) const;

    DesktopSourceActivityConfig config_ {};
    DesktopSourceActivitySnapshot snapshot_ {};
    std::uint64_t current_poll_started_ms_ = 0;
    std::uint64_t reference_last_attempt_ms_ = 0;
    std::uint32_t reference_attempt_ = 0;
    bool capture_stall_reported_ = false;
};

enum class HostStreamWorkReason : std::uint32_t {
    kNone = 0,
    kNewCapture = 1U << 0U,
    kKeyframe = 1U << 1U,
    kViewport = 1U << 2U,
    kRateChange = 1U << 3U,
    kTransportWritable = 1U << 4U,
    kSourceReference = 1U << 5U,
    kStop = 1U << 6U,
    kStateChanged = 1U << 7U,
};

struct HostStreamEncodeReadiness {
    bool channels_ready = false;
    bool source_active = false;
    bool capacity_available = false;
    bool frame_pending = false;
    std::uint64_t due_ms = 0;
};

// An infinite wait means only a state notification may re-enable encoding.
[[nodiscard]] std::chrono::milliseconds host_stream_encode_wait(
    const HostStreamEncodeReadiness& readiness, std::uint64_t now_ms);

struct HostStreamWorkSnapshot {
    std::uint64_t generation = 0;
    std::uint32_t reasons = 0;

    [[nodiscard]] bool contains(HostStreamWorkReason reason) const;
};

class HostStreamWorkCoordinator {
public:
    void post(HostStreamWorkReason reason);
    [[nodiscard]] HostStreamWorkSnapshot snapshot() const;
    [[nodiscard]] HostStreamWorkSnapshot wait_for_change(
        std::uint64_t observed_generation,
        std::chrono::milliseconds timeout);
    [[nodiscard]] HostStreamWorkSnapshot consume(std::uint64_t observed_generation);
    void reset();

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t generation_ = 0;
    std::uint32_t reasons_ = 0;
};

enum class ControllerDecoderRecoveryState {
    kSynchronized,
    kAwaitingIdr,
    kAwaitingDisplayable,
};

enum class ControllerDecoderBreakReason : std::uint32_t {
    kNone = 0,
    kSessionStart = 1U << 0U,
    kFragmentParse = 1U << 1U,
    kFragmentReassembly = 1U << 2U,
    kReassemblyTimeout = 1U << 3U,
    kDependencyFrame = 1U << 4U,
    kDirectPipeBusy = 1U << 5U,
    kDirectPipeFailure = 1U << 6U,
    kSharedSequenceGap = 1U << 7U,
    kSharedSequenceChanged = 1U << 8U,
    kDecodeFailure = 1U << 9U,
};

struct ControllerDecoderRecoveryConfig {
    std::uint64_t minimum_retry_ms = 500;
    std::uint64_t maximum_retry_ms = 2000;
    std::uint64_t rtt_margin_ms = 250;
};

struct ControllerDecoderRecoverySnapshot {
    ControllerDecoderRecoveryState state = ControllerDecoderRecoveryState::kSynchronized;
    std::uint64_t generation = 0;
    std::uint64_t pending_keyframe_id = 0;
    std::uint64_t last_break_frame_id = 0;
    std::uint64_t last_request_ms = 0;
    std::uint64_t retry_delay_ms = 0;
    std::uint32_t request_attempt = 0;
    std::uint32_t reason_mask = 0;
    std::uint64_t dependency_break_total = 0;
    std::uint64_t request_total = 0;
    std::uint64_t retry_total = 0;
    std::uint64_t suppressed_request_total = 0;
    std::uint64_t displayable_ack_total = 0;
    std::uint64_t invalidated_keyframe_total = 0;
};

struct ControllerDecoderRecoveryUpdate {
    bool state_changed = false;
    bool request_keyframe = false;
    ControllerDecoderRecoverySnapshot snapshot {};
};

class ControllerDecoderRecoveryCoordinator {
public:
    explicit ControllerDecoderRecoveryCoordinator(
        ControllerDecoderRecoveryConfig config = {});

    void reset();
    ControllerDecoderRecoveryUpdate on_dependency_break(
        ControllerDecoderBreakReason reason,
        std::uint64_t frame_id,
        std::uint64_t now_ms,
        std::uint32_t srtt_ms);
    ControllerDecoderRecoveryUpdate on_keyframe_submitted(
        std::uint64_t frame_id,
        std::uint64_t now_ms,
        std::uint32_t srtt_ms);
    ControllerDecoderRecoveryUpdate on_displayable_ack(
        std::uint64_t latest_displayable_keyframe_id);
    ControllerDecoderRecoveryUpdate tick(
        std::uint64_t now_ms,
        std::uint32_t srtt_ms);
    [[nodiscard]] bool should_accept_frame(bool keyframe) const;
    [[nodiscard]] ControllerDecoderRecoverySnapshot snapshot() const;

private:
    [[nodiscard]] std::uint64_t base_retry_delay_ms(std::uint32_t srtt_ms) const;
    ControllerDecoderRecoveryUpdate request_keyframe_locked(
        std::uint64_t now_ms,
        std::uint32_t srtt_ms,
        bool retry,
        bool state_changed);
    [[nodiscard]] ControllerDecoderRecoveryUpdate update_locked(
        bool state_changed,
        bool request_keyframe) const;

    ControllerDecoderRecoveryConfig config_ {};
    mutable std::mutex mutex_;
    ControllerDecoderRecoverySnapshot snapshot_ {};
};

enum class IncomingHandshakeDecision {
	accepted,
	rejected_parse,
	rejected_replay,
	rejected_fingerprint,
};

struct IncomingHandshakeResult {
	bool accepted = false;
	IncomingHandshakeDecision decision = IncomingHandshakeDecision::rejected_parse;
	std::string error;
	protocol::HandshakeMessageV1 message {};
};

enum class IncomingUacConsentDecision {
	accepted,
	rejected_parse,
	rejected_session_mismatch,
};

struct IncomingUacConsentResult {
	bool accepted = false;
	IncomingUacConsentDecision decision = IncomingUacConsentDecision::rejected_parse;
	std::string error;
	protocol::UacConsentEnvelopeV1 message {};
	service::UacConsentResult applied_result {};
};

class IncomingHandshakeProcessor {
public:
	IncomingHandshakeProcessor(
		security::InMemoryHandshakeReplayGuard& replay_guard,
		security::InMemoryPeerFingerprintVerifier& fingerprint_verifier);

	IncomingHandshakeResult process(std::string_view serialized_handshake);
	void reset_session(std::string_view session_id);

private:
	security::InMemoryHandshakeReplayGuard& replay_guard_;
	security::InMemoryPeerFingerprintVerifier& fingerprint_verifier_;
};

class FingerprintTrustPolicyRuntime {
public:
	enum class LoadFallbackPolicy {
		kRetainExistingOnError,
		kClearOnError,
	};

	FingerprintTrustPolicyRuntime(
		security::InMemoryPeerFingerprintVerifier& verifier,
		security::FileBackedPeerFingerprintStore& store);

	bool load_on_startup(
		LoadFallbackPolicy fallback_policy = LoadFallbackPolicy::kClearOnError,
		std::string* error = nullptr);
	bool reload(
		LoadFallbackPolicy fallback_policy = LoadFallbackPolicy::kRetainExistingOnError,
		std::string* error = nullptr);

private:
	bool apply_policy(
		LoadFallbackPolicy fallback_policy,
		std::string* error);

	security::InMemoryPeerFingerprintVerifier& verifier_;
	security::FileBackedPeerFingerprintStore& store_;
};

enum class SessionTransitionEvent {
	kOfferSent,
	kHandshakeValidated,
	kTransportConnected,
	kTransportDisconnected,
	kRecoverySucceeded,
	kRecoveryFailed,
	kTerminateRequested,
};

enum class HandoffState {
	kPreLogin,
	kLaunchingHelper,
	kSynchronizingCapabilities,
	kUserSessionActive,
	kDowngrading,
	kFailed,
};

enum class HandoffTransitionEvent {
	kInteractiveSessionDetected,
	kHelperLaunchSucceeded,
	kHelperLaunchFailed,
	kCapabilitySyncSucceeded,
	kCapabilitySyncFailed,
	kUserLoggedOut,
	kHelperLost,
	kDowngradeCompleted,
	kResetToPreLogin,
};

bool is_valid_session_transition(
	protocol::SessionStateV1 from,
	SessionTransitionEvent event,
	protocol::SessionStateV1 to);

bool is_valid_handoff_transition(
	HandoffState from,
	HandoffTransitionEvent event,
	HandoffState to);

struct HandoffStateUpdate {
	bool transitioned = false;
	HandoffState state = HandoffState::kPreLogin;
	std::string error;
};

struct HandoffAuditEvent {
	std::string session_id;
	HandoffState from = HandoffState::kPreLogin;
	HandoffState to = HandoffState::kPreLogin;
	HandoffTransitionEvent event = HandoffTransitionEvent::kInteractiveSessionDetected;
	bool transitioned = false;
	std::string error;
	std::uint64_t timestamp_unix = 0;
};

using HandoffAuditSink = std::function<void(const HandoffAuditEvent&)>;

[[nodiscard]] std::string format_handoff_audit_log_line(const HandoffAuditEvent& event);

class SessionHandoffStateMachine {
public:
	using NowProvider = std::function<std::uint64_t()>;

	explicit SessionHandoffStateMachine(
		HandoffState initial_state = HandoffState::kPreLogin,
		std::string session_id = {},
		HandoffAuditSink audit_sink = {},
		NowProvider now_provider = {});

	[[nodiscard]] HandoffState state() const;
	[[nodiscard]] HandoffStateUpdate on_event(HandoffTransitionEvent event);

private:
	HandoffStateUpdate apply_transition(HandoffTransitionEvent event, HandoffState to, std::string failure_reason);
	void emit_audit(
		HandoffState from,
		HandoffState to,
		HandoffTransitionEvent event,
		bool transitioned,
		std::string_view error) const;
	static std::uint64_t default_now_unix();

	HandoffState state_;
	std::string session_id_;
	HandoffAuditSink audit_sink_;
	NowProvider now_provider_;
};

struct SessionRecoveryPolicy {
	std::uint32_t recovery_timeout_ms = 5000;
	std::uint32_t max_retry_count = 3;
	std::uint32_t retry_backoff_ms = 250;
};

enum class ConnectivityHealthSignal {
	kConnected,
	kDisconnected,
	kFailed,
};

struct SessionRecoveryUpdate {
	bool transitioned = false;
	protocol::SessionStateV1 state = protocol::SessionStateV1::idle;
	std::string error;
};

class SessionRecoveryOrchestrator {
public:
	using NowProvider = std::function<std::uint64_t()>;

	explicit SessionRecoveryOrchestrator(
		SessionRecoveryPolicy policy = {},
		protocol::SessionStateV1 initial_state = protocol::SessionStateV1::established,
		NowProvider now_provider = {});

	[[nodiscard]] const SessionRecoveryPolicy& policy() const;
	[[nodiscard]] protocol::SessionStateV1 state() const;
	[[nodiscard]] std::uint32_t retry_count() const;
	[[nodiscard]] bool recovery_in_progress() const;

	SessionRecoveryUpdate on_connectivity_signal(ConnectivityHealthSignal signal);
	SessionRecoveryUpdate on_terminate_requested();
	SessionRecoveryUpdate tick();

private:
	SessionRecoveryUpdate apply_transition(
		SessionTransitionEvent event,
		protocol::SessionStateV1 to_state,
		std::string failure_reason);

	SessionRecoveryPolicy policy_;
	protocol::SessionStateV1 state_;
	NowProvider now_provider_;
	std::uint64_t recovery_started_at_ms_ = 0;
	std::uint64_t next_retry_allowed_at_ms_ = 0;
	std::uint32_t retry_count_ = 0;
};

class PrivilegedInputRuntimeOrchestrator {
public:
	using NowProvider = std::function<std::uint64_t()>;
	using SecureChannelAvailabilitySetter = std::function<void(bool)>;

	PrivilegedInputRuntimeOrchestrator(
		input::IInputInjectorBackend& user_desktop_backend,
		input::IInputInjectorBackend& secure_desktop_backend,
		NowProvider now_provider = {},
		SecureChannelAvailabilitySetter secure_channel_setter = {});

	service::IPrivilegedControlBroker& broker();
	input::TargetRoutingInputInjectionAdapter& adapter();
	void on_secure_desktop_channel_ready();
	void on_secure_desktop_channel_lost();
	bool on_session_disconnected(std::string_view session_id);
	void set_secure_desktop_channel_available(bool available);
	[[nodiscard]] bool secure_desktop_channel_available() const;
	[[nodiscard]] bool full_control_enabled() const;
	[[nodiscard]] bool secure_backend_enabled() const;

private:
	void apply_runtime_state() const;

	input::InputPolicyGate policy_gate_;
	input::SecureDesktopBackendGate secure_backend_gate_;
	input::TargetRoutingInputInjectionAdapter adapter_;
	input::CapabilityDrivenInputRuntimeController runtime_controller_;
	service::InMemoryPrivilegedControlBroker broker_;
	SecureChannelAvailabilitySetter secure_channel_setter_;
	bool full_control_enabled_ = false;
	bool secure_channel_available_ = false;
};

class HostSessionLifecycleDispatcher {
public:
	explicit HostSessionLifecycleDispatcher(PrivilegedInputRuntimeOrchestrator& orchestrator);

	void on_host_service_started() const;
	void on_secure_desktop_channel_ready() const;
	void on_secure_desktop_channel_lost() const;
	bool on_transport_disconnected(std::string_view session_id) const;
	bool on_host_service_stopping(std::string_view session_id) const;

private:
	PrivilegedInputRuntimeOrchestrator& orchestrator_;
};

class SessionUacConsentIngressRouter {
public:
	SessionUacConsentIngressRouter(
		service::HostServiceUacConsentSignalProducer& consent_signal_producer,
		std::string expected_session_id);

	IncomingUacConsentResult route(std::string_view serialized_consent);

private:
	service::HostServiceUacConsentSignalProducer& consent_signal_producer_;
	std::string expected_session_id_;
};

void bind_host_service_lifecycle_event_source(
	service::IHostServiceLifecycleEventSource& event_source,
	const HostSessionLifecycleDispatcher& dispatcher);

std::string_view module_name();
}
