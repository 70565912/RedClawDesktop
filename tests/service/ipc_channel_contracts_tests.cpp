#include <iostream>
#include <string>

#include "redclaw/service/ipc_channel_contracts.h"

namespace {

using namespace redclaw::service;

void expect_true(bool condition, const std::string& message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << "\n";
		std::exit(1);
	}
}

void test_handshake_request_round_trip() {
	IpcHandshakeRequest req;
	req.header.schema_version = kIpcSchemaVersionV1;
	req.header.message_type = IpcMessageType::kHandshakeRequest;
	req.header.sequence_number = 42;
	req.header.timestamp_ms = 1234567890ULL;
	req.service_auth_token = "test-token-abc123";
	req.session_id = "session-001";
	req.helper_process_id = "12345";
	req.helper_start_time_ms = 9999999ULL;

	const std::string serialized = serialize_ipc_handshake_request(req);
	expect_true(!serialized.empty(), "handshake request serialized");

	const auto parsed = parse_ipc_handshake_request(serialized);
	expect_true(parsed.ok, "handshake request parsed: " + parsed.error_detail);
	expect_true(parsed.message.header.schema_version == kIpcSchemaVersionV1, "schema version matches");
	expect_true(parsed.message.service_auth_token == "test-token-abc123", "auth token matches");
	expect_true(parsed.message.session_id == "session-001", "session_id matches");
	expect_true(parsed.message.helper_process_id == "12345", "helper_process_id matches");
	expect_true(parsed.message.helper_start_time_ms == 9999999ULL, "helper_start_time_ms matches");
}

void test_handshake_request_validation() {
	IpcHandshakeRequest req;
	req.header.schema_version = kIpcSchemaVersionV1;
	req.service_auth_token = "token";
	req.session_id = "session";
	req.helper_process_id = "pid";

	std::string error;
	expect_true(validate_ipc_handshake_request(req, &error), "valid handshake request passes");

	req.service_auth_token.clear();
	expect_true(!validate_ipc_handshake_request(req, &error), "missing auth token fails");
	expect_true(error.find("auth token") != std::string::npos, "error mentions auth token");

	req.service_auth_token = "token";
	req.session_id.clear();
	expect_true(!validate_ipc_handshake_request(req, &error), "missing session_id fails");
}

void test_handshake_response_round_trip() {
	IpcHandshakeResponse resp;
	resp.header.schema_version = kIpcSchemaVersionV1;
	resp.header.message_type = IpcMessageType::kHandshakeResponse;
	resp.authenticated = true;
	resp.error = IpcErrorCode::kNone;
	resp.session_id = "session-002";
	resp.service_process_id = "9876";

	const std::string serialized = serialize_ipc_handshake_response(resp);
	const auto parsed = parse_ipc_handshake_response(serialized);

	expect_true(parsed.ok, "handshake response parsed");
	expect_true(parsed.message.authenticated, "authenticated flag matches");
	expect_true(parsed.message.error == IpcErrorCode::kNone, "error code matches");
	expect_true(parsed.message.session_id == "session-002", "session_id matches");
	expect_true(parsed.message.service_process_id == "9876", "service_process_id matches");
}

void test_capability_advertisement_round_trip() {
	IpcCapabilityAdvertisement ad;
	ad.header.schema_version = kIpcSchemaVersionV1;
	ad.header.message_type = IpcMessageType::kCapabilityAdvertisement;
	ad.session_id = "session-003";
	ad.available_capabilities = HelperCapabilityFlag::kUserDesktopCapture | 
	                            HelperCapabilityFlag::kClipboardSync;
	ad.feature_flags.push_back("feature-alpha");
	ad.feature_flags.push_back("feature-beta");
	ad.desktop_session_id = 1;

	const std::string serialized = serialize_ipc_capability_advertisement(ad);
	const auto parsed = parse_ipc_capability_advertisement(serialized);

	expect_true(parsed.ok, "capability advertisement parsed");
	expect_true(parsed.message.session_id == "session-003", "session_id matches");
	expect_true(has_capability(parsed.message.available_capabilities, HelperCapabilityFlag::kUserDesktopCapture),
	            "has desktop capture");
	expect_true(has_capability(parsed.message.available_capabilities, HelperCapabilityFlag::kClipboardSync),
	            "has clipboard sync");
	expect_true(parsed.message.feature_flags.size() == 2, "feature flags count matches");
	expect_true(parsed.message.desktop_session_id == 1, "desktop_session_id matches");
}

void test_capability_acknowledgment_round_trip() {
	IpcCapabilityAcknowledgment ack;
	ack.header.schema_version = kIpcSchemaVersionV1;
	ack.header.message_type = IpcMessageType::kCapabilityAcknowledgment;
	ack.accepted = true;
	ack.error = IpcErrorCode::kNone;
	ack.enabled_capabilities = HelperCapabilityFlag::kUserInputInjection;

	const std::string serialized = serialize_ipc_capability_acknowledgment(ack);
	const auto parsed = parse_ipc_capability_acknowledgment(serialized);

	expect_true(parsed.ok, "capability ack parsed");
	expect_true(parsed.message.accepted, "accepted flag matches");
	expect_true(has_capability(parsed.message.enabled_capabilities, HelperCapabilityFlag::kUserInputInjection),
	            "enabled capability matches");
}

void test_session_transfer_round_trip() {
	IpcSessionTransfer transfer;
	transfer.header.schema_version = kIpcSchemaVersionV1;
	transfer.header.message_type = IpcMessageType::kSessionTransfer;
	transfer.session_id = "session-004";
	transfer.transfer_token = "transfer-token-xyz";
	transfer.expires_at_ms = 7777777777ULL;

	const std::string serialized = serialize_ipc_session_transfer(transfer);
	const auto parsed = parse_ipc_session_transfer(serialized);

	expect_true(parsed.ok, "session transfer parsed: " + parsed.error_detail);
	expect_true(parsed.message.session_id == "session-004", "session_id matches");
	expect_true(parsed.message.transfer_token == "transfer-token-xyz", "transfer_token matches");
	expect_true(parsed.message.expires_at_ms == 7777777777ULL, "expires_at_ms matches");
}

void test_keepalive_round_trip() {
	IpcKeepalive keepalive;
	keepalive.header.schema_version = kIpcSchemaVersionV1;
	keepalive.header.message_type = IpcMessageType::kKeepalive;
	keepalive.session_id = "session-005";

	const std::string serialized = serialize_ipc_keepalive(keepalive);
	const auto parsed = parse_ipc_keepalive(serialized);

	expect_true(parsed.ok, "keepalive parsed");
	expect_true(parsed.message.session_id == "session-005", "session_id matches");
}

void test_keepalive_ack_round_trip() {
	IpcKeepaliveAck ack;
	ack.header.schema_version = kIpcSchemaVersionV1;
	ack.header.message_type = IpcMessageType::kKeepaliveAck;
	ack.session_id = "session-006";
	ack.service_uptime_ms = 123456789ULL;

	const std::string serialized = serialize_ipc_keepalive_ack(ack);
	const auto parsed = parse_ipc_keepalive_ack(serialized);

	expect_true(parsed.ok, "keepalive ack parsed");
	expect_true(parsed.message.session_id == "session-006", "session_id matches");
	expect_true(parsed.message.service_uptime_ms == 123456789ULL, "service_uptime_ms matches");
}

void test_shutdown_round_trip() {
	IpcShutdown shutdown;
	shutdown.header.schema_version = kIpcSchemaVersionV1;
	shutdown.header.message_type = IpcMessageType::kShutdown;
	shutdown.session_id = "session-007";
	shutdown.reason_code = "user-logout";
	shutdown.graceful = true;

	const std::string serialized = serialize_ipc_shutdown(shutdown);
	const auto parsed = parse_ipc_shutdown(serialized);

	expect_true(parsed.ok, "shutdown parsed");
	expect_true(parsed.message.session_id == "session-007", "session_id matches");
	expect_true(parsed.message.reason_code == "user-logout", "reason_code matches");
	expect_true(parsed.message.graceful, "graceful flag matches");
}

void test_error_message_round_trip() {
	IpcErrorMessage error_msg;
	error_msg.header.schema_version = kIpcSchemaVersionV1;
	error_msg.header.message_type = IpcMessageType::kError;
	error_msg.error_code = IpcErrorCode::kAuthenticationFailed;
	error_msg.error_detail = "invalid auth token provided";

	const std::string serialized = serialize_ipc_error_message(error_msg);
	const auto parsed = parse_ipc_error_message(serialized);

	expect_true(parsed.ok, "error message parsed");
	expect_true(parsed.message.error_code == IpcErrorCode::kAuthenticationFailed, "error_code matches");
	expect_true(parsed.message.error_detail == "invalid auth token provided", "error_detail matches");
}

void test_malformed_input_rejection() {
	const std::string malformed = "garbage data\nwithout header";
	const auto parsed = parse_ipc_handshake_request(malformed);
	expect_true(!parsed.ok, "malformed input rejected");
	expect_true(!parsed.error_detail.empty(), "error detail provided");
}

void test_invalid_header_rejection() {
	const std::string wrong_header = "WRONG-HEADER-V1\nschema_version=1\n";
	const auto parsed = parse_ipc_handshake_request(wrong_header);
	expect_true(!parsed.ok, "invalid header rejected");
	expect_true(parsed.error_detail.find("header") != std::string::npos, "error mentions header");
}

void test_to_string_helpers() {
	expect_true(to_string(IpcMessageType::kHandshakeRequest) == "HandshakeRequest", 
	            "message type to_string");
	expect_true(to_string(IpcErrorCode::kAuthenticationFailed) == "AuthenticationFailed", 
	            "error code to_string");
	
	const std::string flags_str = to_string(
		HelperCapabilityFlag::kUserDesktopCapture | HelperCapabilityFlag::kClipboardSync);
	expect_true(flags_str.find("UserDesktopCapture") != std::string::npos, 
	            "capability flags to_string includes desktop capture");
	expect_true(flags_str.find("ClipboardSync") != std::string::npos, 
	            "capability flags to_string includes clipboard");
}

void test_capability_flag_operations() {
	const auto flags = HelperCapabilityFlag::kUserDesktopCapture | 
	                   HelperCapabilityFlag::kUserInputInjection;
	
	expect_true(has_capability(flags, HelperCapabilityFlag::kUserDesktopCapture), 
	            "has desktop capture");
	expect_true(has_capability(flags, HelperCapabilityFlag::kUserInputInjection), 
	            "has input injection");
	expect_true(!has_capability(flags, HelperCapabilityFlag::kClipboardSync), 
	            "does not have clipboard sync");
}

void test_escape_and_unescape() {
	IpcHandshakeRequest req;
	req.header.schema_version = kIpcSchemaVersionV1;
	req.header.message_type = IpcMessageType::kHandshakeRequest;
	req.service_auth_token = "token=with\\special\nchars";
	req.session_id = "session";
	req.helper_process_id = "pid";

	const std::string serialized = serialize_ipc_handshake_request(req);
	const auto parsed = parse_ipc_handshake_request(serialized);

	expect_true(parsed.ok, "special chars parsed");
	expect_true(parsed.message.service_auth_token == "token=with\\special\nchars", 
	            "special chars preserved");
}

}  // namespace

int main() {
	std::cout << "Running IPC channel contracts tests...\n";

	test_handshake_request_round_trip();
	test_handshake_request_validation();
	test_handshake_response_round_trip();
	test_capability_advertisement_round_trip();
	test_capability_acknowledgment_round_trip();
	test_session_transfer_round_trip();
	test_keepalive_round_trip();
	test_keepalive_ack_round_trip();
	test_shutdown_round_trip();
	test_error_message_round_trip();
	test_malformed_input_rejection();
	test_invalid_header_rejection();
	test_to_string_helpers();
	test_capability_flag_operations();
	test_escape_and_unescape();

	std::cout << "All IPC channel contracts tests passed!\n";
	return 0;
}
