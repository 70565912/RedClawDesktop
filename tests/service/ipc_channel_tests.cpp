#include <iostream>
#include <memory>
#include <string>

#include "redclaw/service/ipc_channel.h"
#include "redclaw/service/ipc_channel_contracts.h"

namespace {

using namespace redclaw::service;

void expect_true(bool condition, const std::string& message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << "\n";
		std::exit(1);
	}
}

void test_server_creation() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_server_creation";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	expect_true(channel.state() == IpcChannelState::kIdle, "initial state is idle");
	expect_true(channel.start_server(), "server starts successfully");
	expect_true(channel.state() == IpcChannelState::kListening, "state transitions to listening");
}

void test_client_connection() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_client_connection";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	expect_true(channel.state() == IpcChannelState::kIdle, "initial state is idle");
	expect_true(channel.connect_to_server(), "client connects successfully");
	expect_true(channel.state() == IpcChannelState::kConnected, "state transitions to connected");
	expect_true(channel.is_connected(), "is_connected returns true");
}

void test_server_wait_for_client() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_server_wait";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	expect_true(channel.start_server(), "server starts");
	
	// Simulate client connection
	adapter->simulate_client_connect();
	
	expect_true(channel.wait_for_client(100), "wait for client succeeds");
	expect_true(channel.state() == IpcChannelState::kConnected, "state transitions to connected");
}

void test_send_message() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_send";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	channel.connect_to_server();

	const std::string test_message = "Hello IPC!";
	expect_true(channel.send_message(test_message), "send message succeeds");
	expect_true(adapter->last_sent_message() == test_message, "message content matches");
}

void test_receive_message() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_receive";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	channel.connect_to_server();

	const std::string test_message = "Incoming message";
	adapter->simulate_incoming_message(test_message);

	std::string received;
	expect_true(channel.receive_message(received, 100), "receive message succeeds");
	expect_true(received == test_message, "received message matches");
}

void test_message_handler_callback() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_handler";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	std::string captured_message;
	channel.set_message_handler([&captured_message](std::string_view msg) {
		captured_message = std::string(msg);
	});

	channel.connect_to_server();

	const std::string test_message = "Handler test";
	adapter->simulate_incoming_message(test_message);

	std::string received;
	channel.receive_message(received, 100);

	expect_true(captured_message == test_message, "handler callback received message");
}

void test_state_change_callback() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_state_change";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	IpcChannelState last_old_state = IpcChannelState::kIdle;
	IpcChannelState last_new_state = IpcChannelState::kIdle;
	int callback_count = 0;

	channel.set_state_change_handler([&](IpcChannelState old_state, IpcChannelState new_state) {
		last_old_state = old_state;
		last_new_state = new_state;
		callback_count++;
	});

	channel.connect_to_server();

	// Should have transitioned: idle -> connecting -> connected (2 transitions)
	expect_true(callback_count == 2, "state changed twice");
	expect_true(last_old_state == IpcChannelState::kConnecting, "old state was connecting");
	expect_true(last_new_state == IpcChannelState::kConnected, "new state is connected");
}

void test_error_handler_callback() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_error";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	IpcChannelError last_error = IpcChannelError::kNone;
	std::string last_error_detail;

	channel.set_error_handler([&](IpcChannelError error, std::string_view detail) {
		last_error = error;
		last_error_detail = std::string(detail);
	});

	// Try to send without connecting
	channel.send_message("test");

	expect_true(last_error == IpcChannelError::kDisconnected, "error handler called");
	expect_true(!last_error_detail.empty(), "error detail provided");
}

void test_disconnect() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_disconnect";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	channel.connect_to_server();
	expect_true(channel.is_connected(), "connected before disconnect");

	expect_true(channel.disconnect(), "disconnect succeeds");
	expect_true(channel.state() == IpcChannelState::kDisconnected, "state is disconnected");
	expect_true(!channel.is_connected(), "not connected after disconnect");
}

void test_invalid_pipe_name() {
	IpcChannelConfig config;
	config.pipe_name = "";  // Invalid

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	expect_true(!channel.start_server(), "server creation fails with invalid pipe name");
	expect_true(!channel.last_error_detail().empty(), "error detail provided");
}

void test_message_size_limit() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_size_limit";
	config.max_message_size = 100;

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	channel.connect_to_server();

	const std::string large_message(200, 'X');
	expect_true(!channel.send_message(large_message), "large message rejected");
	expect_true(channel.last_error_detail().find("max size") != std::string::npos,
	            "error mentions size limit");
}

void test_round_trip_handshake_messages() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_handshake_round_trip";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	channel.connect_to_server();

	// Create a handshake request
	IpcHandshakeRequest request;
	request.header.schema_version = kIpcSchemaVersionV1;
	request.header.message_type = IpcMessageType::kHandshakeRequest;
	request.header.sequence_number = 1;
	request.service_auth_token = "test-token-123";
	request.session_id = "session-001";
	request.helper_process_id = "9999";

	// Serialize and send
	const std::string serialized = serialize_ipc_handshake_request(request);
	expect_true(channel.send_message(serialized), "send handshake request");

	// Verify it was sent
	expect_true(adapter->last_sent_message() == serialized, "sent message matches");

	// Simulate response
	IpcHandshakeResponse response;
	response.header.schema_version = kIpcSchemaVersionV1;
	response.header.message_type = IpcMessageType::kHandshakeResponse;
	response.authenticated = true;
	response.error = IpcErrorCode::kNone;
	response.session_id = "session-001";

	const std::string response_serialized = serialize_ipc_handshake_response(response);
	adapter->simulate_incoming_message(response_serialized);

	// Receive and parse
	std::string received;
	expect_true(channel.receive_message(received, 100), "receive handshake response");

	const auto parsed = parse_ipc_handshake_response(received);
	expect_true(parsed.ok, "handshake response parsed");
	expect_true(parsed.message.authenticated, "authenticated flag set");
	expect_true(parsed.message.session_id == "session-001", "session_id matches");
}

void test_to_string_helpers() {
	expect_true(to_string(IpcChannelError::kConnectionFailed) == "ConnectionFailed",
	            "error to_string works");
	expect_true(to_string(IpcChannelState::kListening) == "Listening",
	            "state to_string works");
}

}  // namespace

int main() {
	std::cout << "Running IPC channel tests...\n";

	test_server_creation();
	test_client_connection();
	test_server_wait_for_client();
	test_send_message();
	test_receive_message();
	test_message_handler_callback();
	test_state_change_callback();
	test_error_handler_callback();
	test_disconnect();
	test_invalid_pipe_name();
	test_message_size_limit();
	test_round_trip_handshake_messages();
	test_to_string_helpers();

	std::cout << "All IPC channel tests passed!\n";
	return 0;
}
