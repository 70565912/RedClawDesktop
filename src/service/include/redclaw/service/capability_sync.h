#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "redclaw/service/capability_registry.h"
#include "redclaw/service/ipc_channel.h"
#include "redclaw/service/ipc_channel_contracts.h"

namespace redclaw::service {

enum class CapabilitySyncError {
	kNone,
	kNotConnected,
	kInvalidMessage,
	kSyncFailed,
	kCapabilityRejected,
	kTimeout,
	kInternalError,
};

enum class CapabilitySyncState {
	kIdle,
	kAdvertising,
	kNegotiating,
	kSynchronized,
	kFailed,
};

using CapabilitySyncCompleteHandler = std::function<void(
	const CapabilitySet& agreed_capabilities)>;

using CapabilitySyncErrorHandler = std::function<void(
	CapabilitySyncError error,
	std::string_view detail)>;

class IpcCapabilitySyncCoordinator {
public:
	IpcCapabilitySyncCoordinator(
		WindowsNamedPipeIpcChannel& ipc_channel,
		SessionMode initial_mode);

	// Service-side: advertise capabilities to helper
	[[nodiscard]] bool advertise_capabilities(const CapabilitySet& capabilities);

	// Helper-side: send capability advertisement to service
	[[nodiscard]] bool send_advertisement(const CapabilitySet& capabilities);

	// Service-side: acknowledge helper's capabilities
	[[nodiscard]] bool acknowledge_capabilities(
		const CapabilitySet& agreed_capabilities,
		IpcErrorCode error = IpcErrorCode::kNone);

	// Process incoming capability advertisement message
	[[nodiscard]] bool handle_capability_advertisement(const IpcCapabilityAdvertisement& ad);

	// Process incoming capability acknowledgment message
	[[nodiscard]] bool handle_capability_acknowledgment(const IpcCapabilityAcknowledgment& ack);

	void set_sync_complete_handler(CapabilitySyncCompleteHandler handler);
	void set_sync_error_handler(CapabilitySyncErrorHandler handler);

	[[nodiscard]] CapabilitySyncState state() const;
	[[nodiscard]] CapabilitySet current_capabilities() const;
	[[nodiscard]] std::string last_error_detail() const;

private:
	void transition_state(CapabilitySyncState new_state);
	void emit_error(CapabilitySyncError error, std::string_view detail);

	WindowsNamedPipeIpcChannel& ipc_channel_;
	SessionMode session_mode_;
	CapabilitySyncState state_ = CapabilitySyncState::kIdle;
	CapabilitySet current_capabilities_;
	std::string last_error_detail_;

	CapabilitySyncCompleteHandler sync_complete_handler_;
	CapabilitySyncErrorHandler sync_error_handler_;
	std::uint32_t sequence_number_ = 0;
};

// Helper: Build IPC capability advertisement from CapabilitySet
[[nodiscard]] IpcCapabilityAdvertisement build_capability_advertisement(
	const CapabilitySet& caps,
	std::string_view session_id,
	std::uint32_t sequence_number);

// Helper: Build IPC capability acknowledgment
[[nodiscard]] IpcCapabilityAcknowledgment build_capability_acknowledgment(
	const CapabilitySet& agreed_caps,
	bool accepted,
	IpcErrorCode error,
	std::uint32_t sequence_number);

// Helper: Extract CapabilitySet from IPC advertisement
[[nodiscard]] CapabilitySet extract_capability_set_from_advertisement(
	const IpcCapabilityAdvertisement& ad);

// Helper: Extract CapabilitySet from IPC acknowledgment
[[nodiscard]] CapabilitySet extract_capability_set_from_acknowledgment(
	const IpcCapabilityAcknowledgment& ack);

std::string to_string(CapabilitySyncError error);
std::string to_string(CapabilitySyncState state);

}  // namespace redclaw::service
