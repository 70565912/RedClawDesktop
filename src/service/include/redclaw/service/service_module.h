#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace redclaw::service {

enum class CapabilityLevel {
	kViewOnly,
	kStandardControl,
	kFullControl,
};

enum class PrivilegedDecision {
	kAllow,
	kDeny,
	kTimeout,
	kBlocked,
};

enum class PrivilegedError {
	kNone,
	kInvalidSession,
	kPolicyDenied,
	kStepUpRequired,
	kTokenExpired,
	kReplayDetected,
	kSecureDesktopUnavailable,
	kRateLimited,
	kInternalError,
};

enum class PrivilegedState {
	kIdle,
	kStandardActive,
	kPrivilegePending,
	kFullControlActive,
	kUacPromptActive,
	kDeniedLockout,
	kRevoked,
};

enum class PrivilegedTransitionEvent {
	kSessionAuthenticated,
	kRequestPrivilegedControl,
	kVerificationPassed,
	kVerificationFailed,
	kUacPromptRaised,
	kUacPromptHandled,
	kTokenExpired,
	kSessionDisconnected,
	kPolicyUpdated,
	kSecureDesktopFailure,
	kLockoutTriggered,
	kLockoutCooldownComplete,
	kExplicitRevoke,
};

struct StepUpProof {
	std::string operator_id;
	std::string device_fingerprint;
	std::string challenge_id;
	std::string signed_proof;
	std::uint64_t issued_at_unix = 0;
};

struct PrivilegedGrantToken {
	std::string token_id;
	std::string session_id;
	std::string operator_id;
	std::uint64_t expires_at_unix = 0;
	std::string token_signature;
};

struct PrivilegedRequest {
	std::string session_id;
	StepUpProof step_up_proof;
	std::string reason_code;
};

struct PrivilegedRequestResult {
	bool accepted = false;
	PrivilegedError error = PrivilegedError::kInternalError;
	CapabilityLevel granted_level = CapabilityLevel::kStandardControl;
	PrivilegedGrantToken grant_token;
};

struct UacConsentAction {
	std::string session_id;
	std::string token_id;
	PrivilegedDecision decision = PrivilegedDecision::kBlocked;
	std::string uac_prompt_id;
};

struct UacConsentResult {
	bool applied = false;
	PrivilegedError error = PrivilegedError::kInternalError;
	PrivilegedDecision final_decision = PrivilegedDecision::kBlocked;
};

enum class UacPromptLifecycleEventType {
	kRaised,
	kClosed,
	kTimeout,
	kUnavailable,
};

struct UacPromptLifecycleEvent {
	std::string session_id;
	std::string uac_prompt_id;
	UacPromptLifecycleEventType type = UacPromptLifecycleEventType::kUnavailable;
	std::string reason_code;
};

struct PrivilegedAuditEvent {
	std::string session_id;
	std::string operator_id;
	std::string action;
	std::string detail;
	PrivilegedDecision decision = PrivilegedDecision::kBlocked;
	PrivilegedError error = PrivilegedError::kInternalError;
	std::uint64_t timestamp_unix = 0;
};

// Converts broker audit events to M10 structured logs with redaction.
[[nodiscard]] std::string format_privileged_audit_log_line(const PrivilegedAuditEvent& event);

struct CapabilityChangeEvent {
	std::string session_id;
	CapabilityLevel before = CapabilityLevel::kStandardControl;
	CapabilityLevel after = CapabilityLevel::kStandardControl;
	std::string reason;
	std::uint64_t timestamp_unix = 0;
};

enum class ServiceLifecycleError {
	kNone,
	kInvalidConfig,
	kUnsupportedPlatform,
	kCommandFailed,
};

struct ServiceInstallConfig {
	std::string service_name;
	std::string display_name;
	std::string binary_path;
	bool auto_start = true;
	std::string account_name = "LocalSystem";
	std::unordered_map<std::string, std::string> environment_overrides;
};

struct SessionSecurityBootstrapConfig {
	std::string trust_store_path = "trusted-peer-fingerprints.txt";
	std::string trust_store_fallback = "clear";
};

enum class RendezvousRegistryError {
	kNone,
	kInvalidRequest,
	kCodeAlreadyExists,
	kCodeNotFound,
	kCodeExpired,
	kCodeAlreadyClaimed,
};

struct RendezvousSessionRegistration {
	std::string session_code;
	std::string session_id;
	std::string host_display_name;
	std::string host_fingerprint_summary;
	std::uint64_t ttl_seconds = 300;
};

struct RendezvousRegisterResult {
	bool accepted = false;
	RendezvousRegistryError error = RendezvousRegistryError::kInvalidRequest;
	std::uint64_t expires_at_unix = 0;
};

struct RendezvousLookupResult {
	bool found = false;
	bool claimed = false;
	RendezvousRegistryError error = RendezvousRegistryError::kCodeNotFound;
	std::string session_id;
	std::string host_display_name;
	std::string host_fingerprint_summary;
	std::uint64_t expires_at_unix = 0;
};

struct RendezvousClaimResult {
	bool claimed = false;
	RendezvousRegistryError error = RendezvousRegistryError::kCodeNotFound;
	std::string session_id;
};

class IRendezvousSessionRegistry {
public:
	virtual ~IRendezvousSessionRegistry() = default;

	[[nodiscard]] virtual RendezvousRegisterResult register_session(const RendezvousSessionRegistration& registration) = 0;
	[[nodiscard]] virtual RendezvousLookupResult lookup_session(std::string_view session_code) = 0;
	[[nodiscard]] virtual RendezvousClaimResult claim_session(std::string_view session_code) = 0;
	virtual void cleanup_expired() = 0;
};

class InMemoryRendezvousSessionRegistry final : public IRendezvousSessionRegistry {
public:
	using NowProvider = std::function<std::uint64_t()>;

	explicit InMemoryRendezvousSessionRegistry(NowProvider now_provider = {});

	[[nodiscard]] RendezvousRegisterResult register_session(const RendezvousSessionRegistration& registration) override;
	[[nodiscard]] RendezvousLookupResult lookup_session(std::string_view session_code) override;
	[[nodiscard]] RendezvousClaimResult claim_session(std::string_view session_code) override;
	void cleanup_expired() override;

private:
	struct RegistryEntry {
		std::string session_id;
		std::string host_display_name;
		std::string host_fingerprint_summary;
		std::uint64_t expires_at_unix = 0;
		bool claimed = false;
	};

	[[nodiscard]] static std::uint64_t default_now();
	[[nodiscard]] static bool is_valid_session_code(std::string_view value);

	NowProvider now_provider_;
	mutable std::mutex mutex_;
	std::unordered_map<std::string, RegistryEntry> entries_;
};

// Resolves runtime session-security bootstrap config from process environment.
// Supported variables:
// - REDCLAW_TRUST_STORE_PATH
// - REDCLAW_TRUST_STORE_FALLBACK (clear|retain)
[[nodiscard]] SessionSecurityBootstrapConfig resolve_session_security_bootstrap_config();

class IWindowsServiceControlAdapter {
public:
	virtual ~IWindowsServiceControlAdapter() = default;

	virtual bool install(const ServiceInstallConfig& config, std::string* error_detail) = 0;
	virtual bool start(std::string_view service_name, std::string* error_detail) = 0;
	virtual bool stop(std::string_view service_name, std::string* error_detail) = 0;
	virtual bool uninstall(std::string_view service_name, std::string* error_detail) = 0;
	virtual bool configure_environment(
		std::string_view service_name,
		const std::unordered_map<std::string, std::string>& environment_overrides,
		std::string* error_detail) = 0;
};

using StepUpVerifier = std::function<bool(const PrivilegedRequest&)>;
using AuditSink = std::function<void(const PrivilegedAuditEvent&)>;
using CapabilityChangeHook = std::function<void(const CapabilityChangeEvent&)>;

struct HostSessionLifecycleEvents {
	std::function<void()> on_host_service_started;
	std::function<void(std::string_view session_id)> on_secure_desktop_channel_ready;
	std::function<void(std::string_view session_id)> on_secure_desktop_channel_lost;
	std::function<void(std::string_view session_id)> on_transport_disconnected;
	std::function<void(std::string_view session_id)> on_host_service_stopping;
};

class IHostServiceLifecycleEventSource {
public:
	virtual ~IHostServiceLifecycleEventSource() = default;

	virtual void set_events(HostSessionLifecycleEvents events) = 0;
	[[nodiscard]] virtual bool start_host_service() = 0;
	[[nodiscard]] virtual bool stop_host_service(std::string_view session_id) = 0;
	[[nodiscard]] virtual bool running() const = 0;
};

// Callback-backed source that can be driven by real service/session event callbacks.
class CallbackDrivenHostServiceLifecycleEventSource final : public IHostServiceLifecycleEventSource {
public:
	void set_events(HostSessionLifecycleEvents events) override;

	[[nodiscard]] bool start_host_service() override;
	[[nodiscard]] bool stop_host_service(std::string_view session_id) override;
	[[nodiscard]] bool running() const override;

	void notify_secure_desktop_channel_ready(std::string_view session_id) const;
	void notify_secure_desktop_channel_lost(std::string_view session_id) const;
	void notify_transport_disconnected(std::string_view session_id) const;

private:
	HostSessionLifecycleEvents events_;
	bool running_ = false;
};

class InMemoryHostServiceLifecycleEmitter final : public IHostServiceLifecycleEventSource {
public:
	void set_events(HostSessionLifecycleEvents events) override;

	[[nodiscard]] bool start_host_service() override;
	[[nodiscard]] bool stop_host_service(std::string_view session_id) override;
	void mark_secure_desktop_channel_ready(std::string_view session_id) const;
	void mark_secure_desktop_channel_lost(std::string_view session_id) const;
	void mark_transport_disconnected(std::string_view session_id) const;

	[[nodiscard]] bool running() const override;

private:
	HostSessionLifecycleEvents events_;
	bool running_ = false;
};

class WindowsServiceLifecycleWrapper {
public:
	explicit WindowsServiceLifecycleWrapper(std::shared_ptr<IWindowsServiceControlAdapter> adapter = {});

	[[nodiscard]] ServiceLifecycleError install(const ServiceInstallConfig& config);
	[[nodiscard]] ServiceLifecycleError start(std::string_view service_name);
	[[nodiscard]] ServiceLifecycleError stop(std::string_view service_name);
	[[nodiscard]] ServiceLifecycleError uninstall(std::string_view service_name);

	[[nodiscard]] std::string last_error_detail() const;

private:
	[[nodiscard]] ServiceLifecycleError run_operation(const std::function<bool(std::string*)>& operation);
	[[nodiscard]] static bool is_valid_service_name(std::string_view service_name);

	std::shared_ptr<IWindowsServiceControlAdapter> adapter_;
	std::string last_error_detail_;
};

// Bridges host service runtime lifecycle with session-facing lifecycle event source.
class HostServiceRuntimeLifecycleBridge {
public:
	HostServiceRuntimeLifecycleBridge(
		WindowsServiceLifecycleWrapper& service_wrapper,
		CallbackDrivenHostServiceLifecycleEventSource& lifecycle_event_source);

	[[nodiscard]] ServiceLifecycleError start(std::string_view service_name);
	[[nodiscard]] ServiceLifecycleError stop(std::string_view service_name, std::string_view session_id);

	void notify_secure_desktop_channel_ready(std::string_view session_id) const;
	void notify_secure_desktop_channel_lost(std::string_view session_id) const;
	void notify_transport_disconnected(std::string_view session_id) const;

	[[nodiscard]] std::string last_error_detail() const;

private:
	WindowsServiceLifecycleWrapper& service_wrapper_;
	CallbackDrivenHostServiceLifecycleEventSource& lifecycle_event_source_;
	std::string last_error_detail_;
};

// Concrete runtime producer that binds M07 service/session signals to bridge notify APIs.
class HostServiceSessionSignalProducer {
public:
	HostServiceSessionSignalProducer(
		HostServiceRuntimeLifecycleBridge& lifecycle_bridge,
		std::string service_name,
		std::string session_id);

	[[nodiscard]] ServiceLifecycleError start_host_service();
	[[nodiscard]] ServiceLifecycleError stop_host_service();

	void on_secure_desktop_channel_ready() const;
	void on_secure_desktop_channel_lost() const;
	void on_transport_disconnected() const;

	[[nodiscard]] std::string last_error_detail() const;

private:
	HostServiceRuntimeLifecycleBridge& lifecycle_bridge_;
	std::string service_name_;
	std::string session_id_;
};

class IPrivilegedControlBroker {
public:
	virtual ~IPrivilegedControlBroker() = default;

	virtual PrivilegedRequestResult requestPrivilegedControl(const PrivilegedRequest& request) = 0;
	virtual bool beginUacPrompt(std::string_view session_id, std::string_view uac_prompt_id) = 0;
	virtual UacConsentResult confirmUacConsent(const UacConsentAction& action) = 0;
	virtual bool reportSecureDesktopUnavailable(std::string_view session_id, std::string_view reason_code) = 0;
	virtual bool revokePrivilegedControl(std::string_view session_id, std::string_view reason_code) = 0;
	virtual CapabilityLevel currentCapability(std::string_view session_id) const = 0;
};

// Deterministic state transition validation for M07-T04.
bool is_valid_transition(PrivilegedState from, PrivilegedTransitionEvent event, PrivilegedState to);

class InMemoryPrivilegedControlBroker final : public IPrivilegedControlBroker {
public:
	using NowProvider = std::function<std::uint64_t()>;

	InMemoryPrivilegedControlBroker(
		NowProvider now_provider = {},
		StepUpVerifier step_up_verifier = {},
		AuditSink audit_sink = {},
		CapabilityChangeHook capability_change_hook = {});

	PrivilegedRequestResult requestPrivilegedControl(const PrivilegedRequest& request) override;
	bool beginUacPrompt(std::string_view session_id, std::string_view uac_prompt_id) override;
	UacConsentResult confirmUacConsent(const UacConsentAction& action) override;
	bool reportSecureDesktopUnavailable(std::string_view session_id, std::string_view reason_code) override;
	bool revokePrivilegedControl(std::string_view session_id, std::string_view reason_code) override;
	CapabilityLevel currentCapability(std::string_view session_id) const override;

private:
	struct SessionContext {
		PrivilegedState state = PrivilegedState::kIdle;
		CapabilityLevel capability = CapabilityLevel::kStandardControl;
		std::string active_token_id;
		std::uint64_t active_token_expiry = 0;
		std::string active_uac_prompt_id;
		std::uint32_t failed_attempts = 0;
		std::unordered_set<std::string> seen_challenges;
		std::unordered_set<std::string> seen_tokens;
	};

	SessionContext& get_or_create_session(const std::string& session_id);
	void emit_audit(
		const std::string& session_id,
		const std::string& operator_id,
		const std::string& action,
		const std::string& detail,
		PrivilegedDecision decision,
		PrivilegedError error) const;
	void emit_capability_change(
		const std::string& session_id,
		CapabilityLevel before,
		CapabilityLevel after,
		const std::string& reason) const;
	static std::uint64_t default_now();
 	static bool default_step_up_verifier(const PrivilegedRequest& request);

	NowProvider now_provider_;
	StepUpVerifier step_up_verifier_;
	AuditSink audit_sink_;
	CapabilityChangeHook capability_change_hook_;
	std::uint64_t token_counter_ = 0;
	std::unordered_map<std::string, SessionContext> sessions_;
};

using UacPromptLifecycleHandler = std::function<void(const UacPromptLifecycleEvent& event)>;
using UacConsentActionHandler = std::function<UacConsentResult(const UacConsentAction& action)>;

class IUacPromptLifecycleEventSource {
public:
	virtual ~IUacPromptLifecycleEventSource() = default;
	virtual void set_handler(UacPromptLifecycleHandler handler) = 0;
};

class IWindowsUacPromptEventAdapter {
public:
	virtual ~IWindowsUacPromptEventAdapter() = default;
	virtual void set_event_sink(UacPromptLifecycleHandler sink) = 0;
	[[nodiscard]] virtual bool start(std::string* error_detail) = 0;
	[[nodiscard]] virtual bool stop(std::string* error_detail) = 0;
};

// OS-level lifecycle source backed by a Windows adapter and normalized event sink.
class WindowsUacPromptLifecycleEventSource final : public IUacPromptLifecycleEventSource {
public:
	explicit WindowsUacPromptLifecycleEventSource(std::shared_ptr<IWindowsUacPromptEventAdapter> adapter = {});

	void set_handler(UacPromptLifecycleHandler handler) override;
	[[nodiscard]] bool start_monitoring();
	[[nodiscard]] bool stop_monitoring();
	[[nodiscard]] bool running() const;
	[[nodiscard]] std::string last_error_detail() const;

private:
	std::shared_ptr<IWindowsUacPromptEventAdapter> adapter_;
	UacPromptLifecycleHandler handler_;
	bool running_ = false;
	std::string last_error_detail_;
};

class IUacConsentActionTransport {
public:
	virtual ~IUacConsentActionTransport() = default;
	virtual void set_handler(UacConsentActionHandler handler) = 0;
	[[nodiscard]] virtual UacConsentResult submit(const UacConsentAction& action) const = 0;
};

class InMemoryUacPromptLifecycleEventSource final : public IUacPromptLifecycleEventSource {
public:
	void set_handler(UacPromptLifecycleHandler handler) override;

	void emit_prompt_raised(std::string_view session_id, std::string_view uac_prompt_id) const;
	void emit_prompt_closed(std::string_view session_id, std::string_view uac_prompt_id) const;
	void emit_prompt_timeout(std::string_view session_id, std::string_view uac_prompt_id) const;
	void emit_prompt_unavailable(
		std::string_view session_id,
		std::string_view uac_prompt_id,
		std::string_view reason_code) const;

private:
	void dispatch_event(const UacPromptLifecycleEvent& event) const;

	UacPromptLifecycleHandler handler_;
};

class InMemoryUacConsentActionTransport final : public IUacConsentActionTransport {
public:
	void set_handler(UacConsentActionHandler handler) override;
	[[nodiscard]] UacConsentResult submit(const UacConsentAction& action) const override;

private:
	UacConsentActionHandler handler_;
};

// Session-scoped producer that binds runtime UAC decisions to consent transport.
class HostServiceUacConsentSignalProducer {
public:
	HostServiceUacConsentSignalProducer(
		IUacConsentActionTransport& consent_transport,
		std::string session_id);

	[[nodiscard]] UacConsentResult submit_allow(std::string_view token_id, std::string_view uac_prompt_id) const;
	[[nodiscard]] UacConsentResult submit_deny(std::string_view token_id, std::string_view uac_prompt_id) const;
	[[nodiscard]] UacConsentResult submit_timeout(std::string_view token_id, std::string_view uac_prompt_id) const;

private:
	[[nodiscard]] UacConsentResult submit(
		std::string_view token_id,
		std::string_view uac_prompt_id,
		PrivilegedDecision decision) const;

	IUacConsentActionTransport& consent_transport_;
	std::string session_id_;
};

// Connects UAC lifecycle source and consent transport to privileged broker semantics.
class HostServiceUacPromptRuntimeCoordinator {
public:
	HostServiceUacPromptRuntimeCoordinator(
		IPrivilegedControlBroker& broker,
		IUacPromptLifecycleEventSource& lifecycle_source,
		IUacConsentActionTransport& consent_transport);

	[[nodiscard]] bool last_prompt_event_applied() const;

private:
	void handle_prompt_event(const UacPromptLifecycleEvent& event);
	[[nodiscard]] UacConsentResult handle_consent_action(const UacConsentAction& action) const;

	IPrivilegedControlBroker& broker_;
	bool last_prompt_event_applied_ = false;
};

std::string_view module_name();
}
