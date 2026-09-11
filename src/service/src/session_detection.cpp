#include "redclaw/service/session_detection.h"

namespace redclaw::service {

namespace {
std::shared_ptr<ISessionNotificationAdapter> create_default_adapter() {
	return std::make_shared<InMemorySessionNotificationAdapter>();
}
}  // namespace

bool InMemorySessionNotificationAdapter::register_notification(std::string* error_detail) {
	if (running_) {
		if (error_detail) {
			*error_detail = "already registered";
		}
		return false;
	}

	running_ = true;
	if (error_detail) {
		error_detail->clear();
	}
	return true;
}

bool InMemorySessionNotificationAdapter::unregister_notification(std::string* error_detail) {
	if (!running_) {
		if (error_detail) {
			*error_detail = "not registered";
		}
		return false;
	}

	running_ = false;
	if (error_detail) {
		error_detail->clear();
	}
	return true;
}

bool InMemorySessionNotificationAdapter::running() const {
	return running_;
}

SessionDetectionListener::SessionDetectionListener(std::shared_ptr<ISessionNotificationAdapter> adapter)
	: adapter_(adapter ? std::move(adapter) : create_default_adapter()) {}

SessionDetectionError SessionDetectionListener::start() {
	if (adapter_->running()) {
		last_error_detail_ = "session listener already started";
		return SessionDetectionError::kAlreadyStarted;
	}

	std::string error_detail;
	if (!adapter_->register_notification(&error_detail)) {
		last_error_detail_ = error_detail;
		return SessionDetectionError::kRegistrationFailed;
	}

	last_error_detail_.clear();
	return SessionDetectionError::kNone;
}

SessionDetectionError SessionDetectionListener::stop() {
	if (!adapter_->running()) {
		last_error_detail_ = "session listener not started";
		return SessionDetectionError::kNotStarted;
	}

	std::string error_detail;
	if (!adapter_->unregister_notification(&error_detail)) {
		last_error_detail_ = error_detail;
		return SessionDetectionError::kUnregistrationFailed;
	}

	last_error_detail_.clear();
	return SessionDetectionError::kNone;
}

bool SessionDetectionListener::is_running() const {
	return adapter_->running();
}

void SessionDetectionListener::set_session_change_handler(SessionChangeHandler handler) {
	handler_ = std::move(handler);
}

std::string SessionDetectionListener::last_error_detail() const {
	return last_error_detail_;
}

void SessionDetectionListener::handle_session_change(std::uint32_t event_code, std::uint32_t session_id) {
	emit_event(map_event_code(event_code), session_id);
}

SessionEventType SessionDetectionListener::map_event_code(std::uint32_t event_code) {
	switch (event_code) {
		case 0x5:  // WTS_SESSION_LOGON
			return SessionEventType::kLogon;
		case 0x6:  // WTS_SESSION_LOGOFF
			return SessionEventType::kLogoff;
		case 0x7:  // WTS_SESSION_LOCK
			return SessionEventType::kLock;
		case 0x8:  // WTS_SESSION_UNLOCK
			return SessionEventType::kUnlock;
		case 0x3:  // WTS_REMOTE_CONNECT
			return SessionEventType::kRemoteConnect;
		case 0x4:  // WTS_REMOTE_DISCONNECT
			return SessionEventType::kRemoteDisconnect;
		default:
			return SessionEventType::kUnknown;
	}
}

void SessionDetectionListener::emit_event(SessionEventType type, std::uint32_t session_id) {
	if (!handler_) {
		return;
	}

	SessionChangeEvent event;
	event.type = type;
	event.session_id = session_id;
	event.source = "WTSRegisterSessionNotification";
	handler_(event);
}

}  // namespace redclaw::service

