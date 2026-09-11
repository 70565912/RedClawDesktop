#include "redclaw/service/capability_sync.h"

#include <sstream>

namespace redclaw::service {

IpcCapabilitySyncCoordinator::IpcCapabilitySyncCoordinator(
	WindowsNamedPipeIpcChannel& ipc_channel,
	SessionMode initial_mode)
	: ipc_channel_(ipc_channel)
	, session_mode_(initial_mode)
	, state_(CapabilitySyncState::kIdle) {
	
	// Set initial capabilities based on mode
	if (initial_mode == SessionMode::kPreLogin) {
		current_capabilities_ = build_pre_login_capability_set();
	} else {
		current_capabilities_ = build_user_session_capability_set();
	}
}

bool IpcCapabilitySyncCoordinator::advertise_capabilities(const CapabilitySet& capabilities) {
	if (!ipc_channel_.is_connected()) {
		last_error_detail_ = "IPC channel not connected";
		emit_error(CapabilitySyncError::kNotConnected, last_error_detail_);
		return false;
	}

	transition_state(CapabilitySyncState::kAdvertising);

	const auto ad = build_capability_advertisement(
		capabilities,
		"service-session",  // TODO: Use actual session ID
		++sequence_number_);

	const std::string serialized = serialize_ipc_capability_advertisement(ad);
	if (!ipc_channel_.send_message(serialized)) {
		last_error_detail_ = "Failed to send capability advertisement: " + ipc_channel_.last_error_detail();
		emit_error(CapabilitySyncError::kSyncFailed, last_error_detail_);
		transition_state(CapabilitySyncState::kFailed);
		return false;
	}

	transition_state(CapabilitySyncState::kNegotiating);
	return true;
}

bool IpcCapabilitySyncCoordinator::send_advertisement(const CapabilitySet& capabilities) {
	// Helper-side advertisement
	return advertise_capabilities(capabilities);
}

bool IpcCapabilitySyncCoordinator::acknowledge_capabilities(
	const CapabilitySet& agreed_capabilities,
	IpcErrorCode error) {

	if (!ipc_channel_.is_connected()) {
		last_error_detail_ = "IPC channel not connected";
		emit_error(CapabilitySyncError::kNotConnected, last_error_detail_);
		return false;
	}

	const bool accepted = (error == IpcErrorCode::kNone);
	const auto ack = build_capability_acknowledgment(
		agreed_capabilities,
		accepted,
		error,
		++sequence_number_);

	const std::string serialized = serialize_ipc_capability_acknowledgment(ack);
	if (!ipc_channel_.send_message(serialized)) {
		last_error_detail_ = "Failed to send capability acknowledgment: " + ipc_channel_.last_error_detail();
		emit_error(CapabilitySyncError::kSyncFailed, last_error_detail_);
		transition_state(CapabilitySyncState::kFailed);
		return false;
	}

	if (accepted) {
		current_capabilities_ = agreed_capabilities;
		transition_state(CapabilitySyncState::kSynchronized);
		
		if (sync_complete_handler_) {
			sync_complete_handler_(agreed_capabilities);
		}
	} else {
		transition_state(CapabilitySyncState::kFailed);
	}

	return true;
}

bool IpcCapabilitySyncCoordinator::handle_capability_advertisement(
	const IpcCapabilityAdvertisement& ad) {

	if (state_ != CapabilitySyncState::kIdle && 
	    state_ != CapabilitySyncState::kNegotiating) {
		last_error_detail_ = "Not in correct state to handle advertisement";
		emit_error(CapabilitySyncError::kInternalError, last_error_detail_);
		return false;
	}

	transition_state(CapabilitySyncState::kNegotiating);

	// Extract capabilities from advertisement
	const CapabilitySet advertised_caps = extract_capability_set_from_advertisement(ad);

	// Validate transition
	if (!is_valid_capability_transition(current_capabilities_, advertised_caps)) {
		last_error_detail_ = "Invalid capability transition";
		emit_error(CapabilitySyncError::kCapabilityRejected, last_error_detail_);
		acknowledge_capabilities(current_capabilities_, IpcErrorCode::kCapabilityRejected);
		return false;
	}

	// Accept capabilities
	current_capabilities_ = advertised_caps;
	acknowledge_capabilities(advertised_caps, IpcErrorCode::kNone);

	return true;
}

bool IpcCapabilitySyncCoordinator::handle_capability_acknowledgment(
	const IpcCapabilityAcknowledgment& ack) {

	if (state_ != CapabilitySyncState::kNegotiating) {
		last_error_detail_ = "Not in negotiating state";
		emit_error(CapabilitySyncError::kInternalError, last_error_detail_);
		return false;
	}

	if (!ack.accepted) {
		last_error_detail_ = "Capabilities rejected by peer: " + to_string(ack.error);
		emit_error(CapabilitySyncError::kCapabilityRejected, last_error_detail_);
		transition_state(CapabilitySyncState::kFailed);
		return false;
	}

	// Extract agreed capabilities
	const CapabilitySet agreed_caps = extract_capability_set_from_acknowledgment(ack);
	current_capabilities_ = agreed_caps;

	transition_state(CapabilitySyncState::kSynchronized);

	if (sync_complete_handler_) {
		sync_complete_handler_(agreed_caps);
	}

	return true;
}

void IpcCapabilitySyncCoordinator::set_sync_complete_handler(
	CapabilitySyncCompleteHandler handler) {
	sync_complete_handler_ = std::move(handler);
}

void IpcCapabilitySyncCoordinator::set_sync_error_handler(
	CapabilitySyncErrorHandler handler) {
	sync_error_handler_ = std::move(handler);
}

CapabilitySyncState IpcCapabilitySyncCoordinator::state() const {
	return state_;
}

CapabilitySet IpcCapabilitySyncCoordinator::current_capabilities() const {
	return current_capabilities_;
}

std::string IpcCapabilitySyncCoordinator::last_error_detail() const {
	return last_error_detail_;
}

void IpcCapabilitySyncCoordinator::transition_state(CapabilitySyncState new_state) {
	state_ = new_state;
}

void IpcCapabilitySyncCoordinator::emit_error(
	CapabilitySyncError error,
	std::string_view detail) {
	
	if (sync_error_handler_) {
		sync_error_handler_(error, detail);
	}
}

// Helper functions

IpcCapabilityAdvertisement build_capability_advertisement(
	const CapabilitySet& caps,
	std::string_view session_id,
	std::uint32_t sequence_number) {

	IpcCapabilityAdvertisement ad;
	ad.header.schema_version = kIpcSchemaVersionV1;
	ad.header.message_type = IpcMessageType::kCapabilityAdvertisement;
	ad.header.sequence_number = sequence_number;
	ad.session_id = std::string(session_id);
	ad.available_capabilities = caps.helper_capabilities;

	// Add feature flags
	if (caps.user_desktop_capture) {
		ad.feature_flags.push_back("user_desktop_capture");
	}
	if (caps.user_desktop_input) {
		ad.feature_flags.push_back("user_desktop_input");
	}
	if (caps.secure_desktop_capture) {
		ad.feature_flags.push_back("secure_desktop_capture");
	}
	if (caps.secure_desktop_input) {
		ad.feature_flags.push_back("secure_desktop_input");
	}
	if (caps.clipboard_sync) {
		ad.feature_flags.push_back("clipboard_sync");
	}
	if (caps.file_transfer) {
		ad.feature_flags.push_back("file_transfer");
	}
	if (caps.notifications) {
		ad.feature_flags.push_back("notifications");
	}
	if (caps.audio_capture) {
		ad.feature_flags.push_back("audio_capture");
	}

	return ad;
}

IpcCapabilityAcknowledgment build_capability_acknowledgment(
	const CapabilitySet& agreed_caps,
	bool accepted,
	IpcErrorCode error,
	std::uint32_t sequence_number) {

	IpcCapabilityAcknowledgment ack;
	ack.header.schema_version = kIpcSchemaVersionV1;
	ack.header.message_type = IpcMessageType::kCapabilityAcknowledgment;
	ack.header.sequence_number = sequence_number;
	ack.accepted = accepted;
	ack.error = error;
	ack.enabled_capabilities = agreed_caps.helper_capabilities;

	return ack;
}

CapabilitySet extract_capability_set_from_advertisement(
	const IpcCapabilityAdvertisement& ad) {

	CapabilitySet caps;
	caps.helper_capabilities = ad.available_capabilities;

	// Parse feature flags
	for (const auto& flag : ad.feature_flags) {
		if (flag == "user_desktop_capture") {
			caps.user_desktop_capture = true;
		} else if (flag == "user_desktop_input") {
			caps.user_desktop_input = true;
		} else if (flag == "secure_desktop_capture") {
			caps.secure_desktop_capture = true;
		} else if (flag == "secure_desktop_input") {
			caps.secure_desktop_input = true;
		} else if (flag == "clipboard_sync") {
			caps.clipboard_sync = true;
		} else if (flag == "file_transfer") {
			caps.file_transfer = true;
		} else if (flag == "notifications") {
			caps.notifications = true;
		} else if (flag == "audio_capture") {
			caps.audio_capture = true;
		}
	}

	// Determine session mode from capabilities
	if (caps.user_desktop_capture || caps.clipboard_sync) {
		caps.session_mode = SessionMode::kUserSession;
	} else {
		caps.session_mode = SessionMode::kPreLogin;
	}

	return caps;
}

CapabilitySet extract_capability_set_from_acknowledgment(
	const IpcCapabilityAcknowledgment& ack) {

	CapabilitySet caps;
	caps.helper_capabilities = ack.enabled_capabilities;

	// Infer basic capabilities from helper flags
	if (has_capability(ack.enabled_capabilities, HelperCapabilityFlag::kUserDesktopCapture)) {
		caps.user_desktop_capture = true;
		caps.session_mode = SessionMode::kUserSession;
	}
	if (has_capability(ack.enabled_capabilities, HelperCapabilityFlag::kUserInputInjection)) {
		caps.user_desktop_input = true;
	}
	if (has_capability(ack.enabled_capabilities, HelperCapabilityFlag::kClipboardSync)) {
		caps.clipboard_sync = true;
	}
	if (has_capability(ack.enabled_capabilities, HelperCapabilityFlag::kFileTransfer)) {
		caps.file_transfer = true;
	}
	if (has_capability(ack.enabled_capabilities, HelperCapabilityFlag::kNotifications)) {
		caps.notifications = true;
	}

	return caps;
}

std::string to_string(CapabilitySyncError error) {
	switch (error) {
		case CapabilitySyncError::kNone: return "None";
		case CapabilitySyncError::kNotConnected: return "NotConnected";
		case CapabilitySyncError::kInvalidMessage: return "InvalidMessage";
		case CapabilitySyncError::kSyncFailed: return "SyncFailed";
		case CapabilitySyncError::kCapabilityRejected: return "CapabilityRejected";
		case CapabilitySyncError::kTimeout: return "Timeout";
		case CapabilitySyncError::kInternalError: return "InternalError";
		default: return "Unknown";
	}
}

std::string to_string(CapabilitySyncState state) {
	switch (state) {
		case CapabilitySyncState::kIdle: return "Idle";
		case CapabilitySyncState::kAdvertising: return "Advertising";
		case CapabilitySyncState::kNegotiating: return "Negotiating";
		case CapabilitySyncState::kSynchronized: return "Synchronized";
		case CapabilitySyncState::kFailed: return "Failed";
		default: return "Unknown";
	}
}

}  // namespace redclaw::service
