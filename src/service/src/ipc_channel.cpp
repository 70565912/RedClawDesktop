#include "redclaw/service/ipc_channel.h"

#include <algorithm>

namespace redclaw::service {

namespace {

std::shared_ptr<IWindowsNamedPipeAdapter> create_default_adapter() {
	return std::make_shared<InMemoryNamedPipeAdapter>();
}

}  // namespace

// WindowsNamedPipeIpcChannel implementation

WindowsNamedPipeIpcChannel::WindowsNamedPipeIpcChannel(
	IpcChannelConfig config,
	std::shared_ptr<IWindowsNamedPipeAdapter> adapter)
	: config_(std::move(config))
	, adapter_(adapter ? std::move(adapter) : create_default_adapter())
	, state_(IpcChannelState::kIdle) {
}

WindowsNamedPipeIpcChannel::~WindowsNamedPipeIpcChannel() {
	if (state_ == IpcChannelState::kConnected || state_ == IpcChannelState::kListening) {
		disconnect();
	}
}

bool WindowsNamedPipeIpcChannel::start_server() {
	if (state_ != IpcChannelState::kIdle) {
		last_error_detail_ = "channel not in idle state";
		emit_error(IpcChannelError::kInternalError, last_error_detail_);
		return false;
	}

	if (config_.pipe_name.empty()) {
		last_error_detail_ = "invalid pipe name";
		emit_error(IpcChannelError::kInvalidPipeName, last_error_detail_);
		return false;
	}

	std::string error_detail;
	if (!adapter_->create_pipe(config_.pipe_name, config_.buffer_size, &error_detail)) {
		last_error_detail_ = "pipe creation failed: " + error_detail;
		emit_error(IpcChannelError::kPipeCreationFailed, last_error_detail_);
		transition_state(IpcChannelState::kError);
		return false;
	}

	transition_state(IpcChannelState::kListening);
	return true;
}

bool WindowsNamedPipeIpcChannel::connect_to_server() {
	if (state_ != IpcChannelState::kIdle) {
		last_error_detail_ = "channel not in idle state";
		emit_error(IpcChannelError::kInternalError, last_error_detail_);
		return false;
	}

	transition_state(IpcChannelState::kConnecting);

	std::string error_detail;
	if (!adapter_->connect_pipe(config_.pipe_name, config_.timeout_ms, &error_detail)) {
		last_error_detail_ = "connection failed: " + error_detail;
		emit_error(IpcChannelError::kConnectionFailed, last_error_detail_);
		transition_state(IpcChannelState::kError);
		return false;
	}

	transition_state(IpcChannelState::kConnected);
	return true;
}

bool WindowsNamedPipeIpcChannel::wait_for_client(std::uint32_t timeout_ms) {
	if (state_ != IpcChannelState::kListening) {
		last_error_detail_ = "channel not listening";
		emit_error(IpcChannelError::kInternalError, last_error_detail_);
		return false;
	}

	const std::uint32_t actual_timeout = timeout_ms != 0 ? timeout_ms : config_.timeout_ms;

	std::string error_detail;
	if (!adapter_->wait_for_connection(actual_timeout, &error_detail)) {
		last_error_detail_ = "wait for connection failed: " + error_detail;
		emit_error(IpcChannelError::kTimeout, last_error_detail_);
		return false;
	}

	transition_state(IpcChannelState::kConnected);
	return true;
}

bool WindowsNamedPipeIpcChannel::disconnect() {
	if (state_ != IpcChannelState::kConnected && state_ != IpcChannelState::kListening) {
		return true;  // Already disconnected
	}

	std::string error_detail;
	if (!adapter_->disconnect(&error_detail)) {
		last_error_detail_ = "disconnect failed: " + error_detail;
		emit_error(IpcChannelError::kInternalError, last_error_detail_);
	}

	transition_state(IpcChannelState::kDisconnected);
	return true;
}

bool WindowsNamedPipeIpcChannel::send_message(std::string_view message) {
	if (state_ != IpcChannelState::kConnected) {
		last_error_detail_ = "channel not connected";
		emit_error(IpcChannelError::kDisconnected, last_error_detail_);
		return false;
	}

	if (message.size() > config_.max_message_size) {
		last_error_detail_ = "message exceeds max size";
		emit_error(IpcChannelError::kBufferOverflow, last_error_detail_);
		return false;
	}

	std::string error_detail;
	if (!adapter_->write_message(message, &error_detail)) {
		last_error_detail_ = "write failed: " + error_detail;
		emit_error(IpcChannelError::kWriteFailed, last_error_detail_);
		transition_state(IpcChannelState::kError);
		return false;
	}

	return true;
}

bool WindowsNamedPipeIpcChannel::receive_message(std::string& out_message, std::uint32_t timeout_ms) {
	if (state_ != IpcChannelState::kConnected) {
		last_error_detail_ = "channel not connected";
		emit_error(IpcChannelError::kDisconnected, last_error_detail_);
		return false;
	}

	const std::uint32_t actual_timeout = timeout_ms != 0 ? timeout_ms : config_.timeout_ms;

	std::string error_detail;
	if (!adapter_->read_message(out_message, actual_timeout, &error_detail)) {
		last_error_detail_ = "read failed: " + error_detail;
		emit_error(IpcChannelError::kReadFailed, last_error_detail_);
		if (error_detail.find("disconnected") != std::string::npos) {
			transition_state(IpcChannelState::kDisconnected);
		}
		return false;
	}

	if (message_handler_) {
		message_handler_(out_message);
	}

	return true;
}

void WindowsNamedPipeIpcChannel::set_message_handler(IpcMessageHandler handler) {
	message_handler_ = std::move(handler);
}

void WindowsNamedPipeIpcChannel::set_error_handler(IpcErrorHandler handler) {
	error_handler_ = std::move(handler);
}

void WindowsNamedPipeIpcChannel::set_state_change_handler(IpcStateChangeHandler handler) {
	state_change_handler_ = std::move(handler);
}

IpcChannelState WindowsNamedPipeIpcChannel::state() const {
	return state_;
}

bool WindowsNamedPipeIpcChannel::is_connected() const {
	return state_ == IpcChannelState::kConnected && adapter_->is_connected();
}

std::string WindowsNamedPipeIpcChannel::last_error_detail() const {
	return last_error_detail_;
}

void WindowsNamedPipeIpcChannel::transition_state(IpcChannelState new_state) {
	const IpcChannelState old_state = state_;
	if (old_state == new_state) {
		return;
	}

	state_ = new_state;

	if (state_change_handler_) {
		state_change_handler_(old_state, new_state);
	}
}

void WindowsNamedPipeIpcChannel::emit_error(IpcChannelError error, std::string_view detail) {
	if (error_handler_) {
		error_handler_(error, detail);
	}
}

// InMemoryNamedPipeAdapter implementation

bool InMemoryNamedPipeAdapter::create_pipe(
	std::string_view pipe_name,
	std::uint32_t buffer_size,
	std::string* error_detail) {
	
	if (mode_ != Mode::kNone) {
		if (error_detail) *error_detail = "adapter already in use";
		return false;
	}

	if (pipe_name.empty()) {
		if (error_detail) *error_detail = "invalid pipe name";
		return false;
	}

	mode_ = Mode::kServer;
	pipe_name_ = std::string(pipe_name);
	connected_ = false;

	if (error_detail) error_detail->clear();
	return true;
}

bool InMemoryNamedPipeAdapter::connect_pipe(
	std::string_view pipe_name,
	std::uint32_t timeout_ms,
	std::string* error_detail) {
	
	if (mode_ != Mode::kNone) {
		if (error_detail) *error_detail = "adapter already in use";
		return false;
	}

	if (pipe_name.empty()) {
		if (error_detail) *error_detail = "invalid pipe name";
		return false;
	}

	mode_ = Mode::kClient;
	pipe_name_ = std::string(pipe_name);
	connected_ = true;  // In-memory connection succeeds immediately

	if (error_detail) error_detail->clear();
	return true;
}

bool InMemoryNamedPipeAdapter::wait_for_connection(std::uint32_t timeout_ms, std::string* error_detail) {
	if (mode_ != Mode::kServer) {
		if (error_detail) *error_detail = "not in server mode";
		return false;
	}

	if (connected_) {
		if (error_detail) error_detail->clear();
		return true;
	}

	// In test mode, connection must be simulated
	if (error_detail) *error_detail = "timeout waiting for connection";
	return false;
}

bool InMemoryNamedPipeAdapter::read_message(
	std::string& out_message,
	std::uint32_t timeout_ms,
	std::string* error_detail) {
	
	if (!connected_) {
		if (error_detail) *error_detail = "not connected";
		return false;
	}

	if (pending_message_.empty()) {
		if (error_detail) *error_detail = "no message available";
		return false;
	}

	out_message = pending_message_;
	pending_message_.clear();

	if (error_detail) error_detail->clear();
	return true;
}

bool InMemoryNamedPipeAdapter::write_message(std::string_view message, std::string* error_detail) {
	if (!connected_) {
		if (error_detail) *error_detail = "not connected";
		return false;
	}

	last_sent_message_ = std::string(message);

	if (error_detail) error_detail->clear();
	return true;
}

bool InMemoryNamedPipeAdapter::disconnect(std::string* error_detail) {
	connected_ = false;
	mode_ = Mode::kNone;
	pipe_name_.clear();
	pending_message_.clear();

	if (error_detail) error_detail->clear();
	return true;
}

bool InMemoryNamedPipeAdapter::is_connected() const {
	return connected_;
}

void InMemoryNamedPipeAdapter::simulate_client_connect() {
	if (mode_ == Mode::kServer) {
		connected_ = true;
	}
}

void InMemoryNamedPipeAdapter::simulate_disconnect() {
	connected_ = false;
}

void InMemoryNamedPipeAdapter::simulate_incoming_message(std::string_view message) {
	pending_message_ = std::string(message);
}

std::string InMemoryNamedPipeAdapter::last_sent_message() const {
	return last_sent_message_;
}

// Helper functions

std::string to_string(IpcChannelError error) {
	switch (error) {
		case IpcChannelError::kNone: return "None";
		case IpcChannelError::kInvalidPipeName: return "InvalidPipeName";
		case IpcChannelError::kPipeCreationFailed: return "PipeCreationFailed";
		case IpcChannelError::kConnectionFailed: return "ConnectionFailed";
		case IpcChannelError::kReadFailed: return "ReadFailed";
		case IpcChannelError::kWriteFailed: return "WriteFailed";
		case IpcChannelError::kDisconnected: return "Disconnected";
		case IpcChannelError::kTimeout: return "Timeout";
		case IpcChannelError::kBufferOverflow: return "BufferOverflow";
		case IpcChannelError::kInternalError: return "InternalError";
		default: return "Unknown";
	}
}

std::string to_string(IpcChannelState state) {
	switch (state) {
		case IpcChannelState::kIdle: return "Idle";
		case IpcChannelState::kListening: return "Listening";
		case IpcChannelState::kConnecting: return "Connecting";
		case IpcChannelState::kConnected: return "Connected";
		case IpcChannelState::kDisconnected: return "Disconnected";
		case IpcChannelState::kError: return "Error";
		default: return "Unknown";
	}
}

}  // namespace redclaw::service
