#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace redclaw::service {

enum class SessionEventType {
	kLogon,
	kLogoff,
	kLock,
	kUnlock,
	kRemoteConnect,
	kRemoteDisconnect,
	kUnknown,
};

struct SessionChangeEvent {
	SessionEventType type = SessionEventType::kUnknown;
	std::uint32_t session_id = 0;
	std::string source;
};

enum class SessionDetectionError {
	kNone,
	kRegistrationFailed,
	kUnregistrationFailed,
	kAlreadyStarted,
	kNotStarted,
};

using SessionChangeHandler = std::function<void(const SessionChangeEvent&)>;

class ISessionNotificationAdapter {
public:
	virtual ~ISessionNotificationAdapter() = default;

	[[nodiscard]] virtual bool register_notification(std::string* error_detail) = 0;
	[[nodiscard]] virtual bool unregister_notification(std::string* error_detail) = 0;
	[[nodiscard]] virtual bool running() const = 0;
};

class InMemorySessionNotificationAdapter final : public ISessionNotificationAdapter {
public:
	[[nodiscard]] bool register_notification(std::string* error_detail) override;
	[[nodiscard]] bool unregister_notification(std::string* error_detail) override;
	[[nodiscard]] bool running() const override;

private:
	bool running_ = false;
};

class SessionDetectionListener {
public:
	explicit SessionDetectionListener(std::shared_ptr<ISessionNotificationAdapter> adapter = {});

	[[nodiscard]] SessionDetectionError start();
	[[nodiscard]] SessionDetectionError stop();
	[[nodiscard]] bool is_running() const;

	void set_session_change_handler(SessionChangeHandler handler);

	[[nodiscard]] std::string last_error_detail() const;

	// For integration with windows callback dispatcher.
	void handle_session_change(std::uint32_t event_code, std::uint32_t session_id);

private:
	[[nodiscard]] static SessionEventType map_event_code(std::uint32_t event_code);
	void emit_event(SessionEventType type, std::uint32_t session_id);

	std::shared_ptr<ISessionNotificationAdapter> adapter_;
	SessionChangeHandler handler_;
	std::string last_error_detail_;
};

}  // namespace redclaw::service

