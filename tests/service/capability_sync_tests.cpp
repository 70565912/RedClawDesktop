#include <iostream>
#include <memory>
#include <string>

#include "redclaw/service/capability_sync.h"

namespace {

using namespace redclaw::service;

void expect_true(bool condition, const std::string& message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << "\n";
		std::exit(1);
	}
}

void test_build_capability_advertisement() {
	const CapabilitySet user_session = build_user_session_capability_set();
	const auto ad = build_capability_advertisement(user_session, "test-session", 1);

	expect_true(ad.header.schema_version == kIpcSchemaVersionV1, "schema version set");
	expect_true(ad.header.message_type == IpcMessageType::kCapabilityAdvertisement, "message type set");
	expect_true(ad.session_id == "test-session", "session ID set");
	expect_true(ad.available_capabilities == user_session.helper_capabilities, "capabilities match");
	expect_true(!ad.feature_flags.empty(), "feature flags populated");
}

void test_extract_capability_set_from_advertisement() {
	const CapabilitySet original = build_user_session_capability_set();
	const auto ad = build_capability_advertisement(original, "test", 1);
	const CapabilitySet extracted = extract_capability_set_from_advertisement(ad);

	expect_true(extracted.user_desktop_capture == original.user_desktop_capture,
	            "user desktop capture matches");
	expect_true(extracted.clipboard_sync == original.clipboard_sync,
	            "clipboard sync matches");
	expect_true(extracted.session_mode == SessionMode::kUserSession,
	            "session mode inferred correctly");
}

void test_build_capability_acknowledgment() {
	const CapabilitySet caps = build_user_session_capability_set();
	const auto ack = build_capability_acknowledgment(caps, true, IpcErrorCode::kNone, 2);

	expect_true(ack.header.message_type == IpcMessageType::kCapabilityAcknowledgment,
	            "message type set");
	expect_true(ack.accepted, "accepted flag set");
	expect_true(ack.error == IpcErrorCode::kNone, "no error");
	expect_true(ack.enabled_capabilities == caps.helper_capabilities, "capabilities match");
}

void test_sync_coordinator_initial_state() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_sync_initial";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	IpcCapabilitySyncCoordinator sync(channel, SessionMode::kPreLogin);

	expect_true(sync.state() == CapabilitySyncState::kIdle, "initial state is idle");
	
	const auto caps = sync.current_capabilities();
	expect_true(caps.session_mode == SessionMode::kPreLogin, "initial mode is pre-login");
}

void test_advertise_capabilities_not_connected() {
	IpcChannelConfig config;
	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	IpcCapabilitySyncCoordinator sync(channel, SessionMode::kPreLogin);

	const CapabilitySet caps = build_user_session_capability_set();
	expect_true(!sync.advertise_capabilities(caps), "advertise fails when not connected");
	expect_true(!sync.last_error_detail().empty(), "error detail provided");
}

void test_advertise_capabilities_success() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_sync_advertise";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);
	channel.connect_to_server();

	IpcCapabilitySyncCoordinator sync(channel, SessionMode::kPreLogin);

	const CapabilitySet caps = build_user_session_capability_set();
	expect_true(sync.advertise_capabilities(caps), "advertise succeeds when connected");
	expect_true(sync.state() == CapabilitySyncState::kNegotiating, "state transitions to negotiating");

	// Verify message was sent
	const std::string sent = adapter->last_sent_message();
	expect_true(!sent.empty(), "message was sent");
	expect_true(sent.find("RCD-IPC-CAPABILITY-AD-V1") != std::string::npos,
	            "sent capability advertisement");
}

void test_handle_capability_advertisement() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_sync_handle_ad";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);
	channel.connect_to_server();

	IpcCapabilitySyncCoordinator sync(channel, SessionMode::kPreLogin);

	// Create advertisement
	const CapabilitySet user_caps = build_user_session_capability_set();
	IpcCapabilityAdvertisement ad = build_capability_advertisement(user_caps, "test", 1);

	expect_true(sync.handle_capability_advertisement(ad), "handle advertisement succeeds");
	expect_true(sync.state() == CapabilitySyncState::kSynchronized, "state is synchronized");

	// Verify acknowledgment was sent
	const std::string sent = adapter->last_sent_message();
	expect_true(sent.find("RCD-IPC-CAPABILITY-ACK-V1") != std::string::npos,
	            "sent acknowledgment");
}

void test_handle_invalid_capability_transition() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_sync_invalid";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);
	channel.connect_to_server();

	IpcCapabilitySyncCoordinator sync(channel, SessionMode::kUserSession);
	sync.current_capabilities();  // Start with user session

	// Try to "upgrade" from user session to user session (no change, but test the flow)
	const CapabilitySet user_caps = build_user_session_capability_set();
	IpcCapabilityAdvertisement ad = build_capability_advertisement(user_caps, "test", 1);

	// This should succeed because same-mode transitions are valid
	expect_true(sync.handle_capability_advertisement(ad), "same mode transition allowed");
}

void test_sync_complete_callback() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_sync_callback";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);
	channel.connect_to_server();

	IpcCapabilitySyncCoordinator sync(channel, SessionMode::kPreLogin);

	bool callback_called = false;
	CapabilitySet callback_caps;

	sync.set_sync_complete_handler([&](const CapabilitySet& caps) {
		callback_called = true;
		callback_caps = caps;
	});

	const CapabilitySet user_caps = build_user_session_capability_set();
	IpcCapabilityAdvertisement ad = build_capability_advertisement(user_caps, "test", 1);

	sync.handle_capability_advertisement(ad);

	expect_true(callback_called, "sync complete callback called");
	expect_true(callback_caps.session_mode == SessionMode::kUserSession,
	            "callback received correct capabilities");
}

void test_sync_error_callback() {
	IpcChannelConfig config;
	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);

	IpcCapabilitySyncCoordinator sync(channel, SessionMode::kPreLogin);

	bool error_called = false;
	CapabilitySyncError last_error = CapabilitySyncError::kNone;

	sync.set_sync_error_handler([&](CapabilitySyncError error, std::string_view detail) {
		error_called = true;
		last_error = error;
	});

	// Try to advertise without connection
	const CapabilitySet caps = build_user_session_capability_set();
	sync.advertise_capabilities(caps);

	expect_true(error_called, "error callback called");
	expect_true(last_error == CapabilitySyncError::kNotConnected,
	            "error is NotConnected");
}

void test_handle_capability_acknowledgment_accepted() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_sync_ack";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);
	channel.connect_to_server();

	IpcCapabilitySyncCoordinator sync(channel, SessionMode::kPreLogin);

	// First advertise to get into negotiating state
	const CapabilitySet caps = build_user_session_capability_set();
	sync.advertise_capabilities(caps);

	// Create acceptance acknowledgment
	IpcCapabilityAcknowledgment ack = build_capability_acknowledgment(
		caps, true, IpcErrorCode::kNone, 2);

	expect_true(sync.handle_capability_acknowledgment(ack),
	            "handle acknowledgment succeeds");
	expect_true(sync.state() == CapabilitySyncState::kSynchronized,
	            "state is synchronized");
}

void test_handle_capability_acknowledgment_rejected() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_sync_reject";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);
	channel.connect_to_server();

	IpcCapabilitySyncCoordinator sync(channel, SessionMode::kPreLogin);

	const CapabilitySet caps = build_user_session_capability_set();
	sync.advertise_capabilities(caps);

	// Create rejection acknowledgment
	IpcCapabilityAcknowledgment ack = build_capability_acknowledgment(
		caps, false, IpcErrorCode::kCapabilityRejected, 2);

	expect_true(!sync.handle_capability_acknowledgment(ack),
	            "handle rejection returns false");
	expect_true(sync.state() == CapabilitySyncState::kFailed,
	            "state is failed");
}

void test_to_string_helpers() {
	expect_true(to_string(CapabilitySyncError::kCapabilityRejected) == "CapabilityRejected",
	            "error to_string");
	expect_true(to_string(CapabilitySyncState::kNegotiating) == "Negotiating",
	            "state to_string");
}

void test_round_trip_capability_sync() {
	IpcChannelConfig config;
	config.pipe_name = "\\\\.\\pipe\\test_sync_round_trip";

	auto adapter = std::make_shared<InMemoryNamedPipeAdapter>();
	WindowsNamedPipeIpcChannel channel(config, adapter);
	channel.connect_to_server();

	IpcCapabilitySyncCoordinator sync(channel, SessionMode::kPreLogin);

	// Build and send advertisement
	const CapabilitySet user_caps = build_user_session_capability_set();
	const auto ad = build_capability_advertisement(user_caps, "session-001", 1);
	const std::string ad_serialized = serialize_ipc_capability_advertisement(ad);

	// Parse it back
	const auto ad_parsed = parse_ipc_capability_advertisement(ad_serialized);
	expect_true(ad_parsed.ok, "advertisement parsed successfully");

	// Extract capabilities
	const CapabilitySet extracted = extract_capability_set_from_advertisement(ad_parsed.message);
	expect_true(extracted.user_desktop_capture, "extracted capabilities match");
}

}  // namespace

int main() {
	std::cout << "Running capability sync tests...\n";

	test_build_capability_advertisement();
	test_extract_capability_set_from_advertisement();
	test_build_capability_acknowledgment();
	test_sync_coordinator_initial_state();
	test_advertise_capabilities_not_connected();
	test_advertise_capabilities_success();
	test_handle_capability_advertisement();
	test_handle_invalid_capability_transition();
	test_sync_complete_callback();
	test_sync_error_callback();
	test_handle_capability_acknowledgment_accepted();
	test_handle_capability_acknowledgment_rejected();
	test_to_string_helpers();
	test_round_trip_capability_sync();

	std::cout << "All capability sync tests passed!\n";
	return 0;
}
