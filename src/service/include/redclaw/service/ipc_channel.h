#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "redclaw/service/ipc_channel_contracts.h"

namespace redclaw::service {

enum class IpcChannelError {
	kNone,
	kInvalidPipeName,
	kPipeCreationFailed,
	kConnectionFailed,
	kReadFailed,
	kWriteFailed,
	kDisconnected,
	kTimeout,
	kBufferOverflow,
	kInternalError,
};

enum class IpcChannelState {
	kIdle,
	kListening,
	kConnecting,
	kConnected,
	kDisconnected,
	kError,
};

struct IpcChannelConfig {
	std::string pipe_name = "\\\\.\\pipe\\redclaw_service_ipc";
	std::uint32_t buffer_size = 64 * 1024;  // 64KB
	std::uint32_t timeout_ms = 5000;
	std::uint32_t max_message_size = kMaxIpcMessageSize;
};

using IpcMessageHandler = std::function<void(std::string_view message)>;
using IpcErrorHandler = std::function<void(IpcChannelError error, std::string_view detail)>;
using IpcStateChangeHandler = std::function<void(IpcChannelState old_state, IpcChannelState new_state)>;

class IWindowsNamedPipeAdapter {
public:
	virtual ~IWindowsNamedPipeAdapter() = default;

	virtual bool create_pipe(std::string_view pipe_name, std::uint32_t buffer_size, std::string* error_detail) = 0;
	virtual bool connect_pipe(std::string_view pipe_name, std::uint32_t timeout_ms, std::string* error_detail) = 0;
	virtual bool wait_for_connection(std::uint32_t timeout_ms, std::string* error_detail) = 0;
	virtual bool read_message(std::string& out_message, std::uint32_t timeout_ms, std::string* error_detail) = 0;
	virtual bool write_message(std::string_view message, std::string* error_detail) = 0;
	virtual bool disconnect(std::string* error_detail) = 0;
	virtual bool is_connected() const = 0;
};

class WindowsNamedPipeIpcChannel {
public:
	explicit WindowsNamedPipeIpcChannel(
		IpcChannelConfig config = {},
		std::shared_ptr<IWindowsNamedPipeAdapter> adapter = {});

	~WindowsNamedPipeIpcChannel();

	[[nodiscard]] bool start_server();
	[[nodiscard]] bool connect_to_server();
	[[nodiscard]] bool wait_for_client(std::uint32_t timeout_ms = 0);
	[[nodiscard]] bool disconnect();

	[[nodiscard]] bool send_message(std::string_view message);
	[[nodiscard]] bool receive_message(std::string& out_message, std::uint32_t timeout_ms = 0);

	void set_message_handler(IpcMessageHandler handler);
	void set_error_handler(IpcErrorHandler handler);
	void set_state_change_handler(IpcStateChangeHandler handler);

	[[nodiscard]] IpcChannelState state() const;
	[[nodiscard]] bool is_connected() const;
	[[nodiscard]] std::string last_error_detail() const;

private:
	void transition_state(IpcChannelState new_state);
	void emit_error(IpcChannelError error, std::string_view detail);

	IpcChannelConfig config_;
	std::shared_ptr<IWindowsNamedPipeAdapter> adapter_;
	IpcChannelState state_ = IpcChannelState::kIdle;
	std::string last_error_detail_;

	IpcMessageHandler message_handler_;
	IpcErrorHandler error_handler_;
	IpcStateChangeHandler state_change_handler_;
};

class InMemoryNamedPipeAdapter final : public IWindowsNamedPipeAdapter {
public:
	bool create_pipe(std::string_view pipe_name, std::uint32_t buffer_size, std::string* error_detail) override;
	bool connect_pipe(std::string_view pipe_name, std::uint32_t timeout_ms, std::string* error_detail) override;
	bool wait_for_connection(std::uint32_t timeout_ms, std::string* error_detail) override;
	bool read_message(std::string& out_message, std::uint32_t timeout_ms, std::string* error_detail) override;
	bool write_message(std::string_view message, std::string* error_detail) override;
	bool disconnect(std::string* error_detail) override;
	[[nodiscard]] bool is_connected() const override;

	void simulate_client_connect();
	void simulate_disconnect();
	void simulate_incoming_message(std::string_view message);
	[[nodiscard]] std::string last_sent_message() const;

private:
	enum class Mode { kNone, kServer, kClient };

	Mode mode_ = Mode::kNone;
	bool connected_ = false;
	std::string pipe_name_;
	std::string pending_message_;
	std::string last_sent_message_;
};

std::string to_string(IpcChannelError error);
std::string to_string(IpcChannelState state);

}  // namespace redclaw::service
