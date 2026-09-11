#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace redclaw::service {

inline constexpr int kIpcSchemaVersionV1 = 1;
inline constexpr int kMaxIpcMessageSize = 1024 * 1024;

enum class IpcMessageType {
	kHandshakeRequest,
	kHandshakeResponse,
	kCapabilityAdvertisement,
	kCapabilityAcknowledgment,
	kSessionTransfer,
	kSessionTransferAck,
	kKeepalive,
	kKeepaliveAck,
	kShutdown,
	kShutdownAck,
	kError,
};

enum class IpcErrorCode {
	kNone,
	kInvalidSchema,
	kAuthenticationFailed,
	kInvalidToken,
	kSessionMismatch,
	kCapabilityRejected,
	kTimeout,
	kProtocolViolation,
	kInternalError,
};

enum class HelperCapabilityFlag : std::uint32_t {
	kNone = 0,
	kUserDesktopCapture = 1 << 0,
	kUserInputInjection = 1 << 1,
	kClipboardSync = 1 << 2,
	kFileTransfer = 1 << 3,
	kNotifications = 1 << 4,
};

inline HelperCapabilityFlag operator|(HelperCapabilityFlag a, HelperCapabilityFlag b) {
	return static_cast<HelperCapabilityFlag>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

inline HelperCapabilityFlag operator&(HelperCapabilityFlag a, HelperCapabilityFlag b) {
	return static_cast<HelperCapabilityFlag>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

inline bool has_capability(HelperCapabilityFlag flags, HelperCapabilityFlag check) {
	return (flags & check) == check;
}

struct IpcMessageHeader {
	int schema_version = kIpcSchemaVersionV1;
	IpcMessageType message_type = IpcMessageType::kError;
	std::uint32_t sequence_number = 0;
	std::uint64_t timestamp_ms = 0;
};

struct IpcHandshakeRequest {
	IpcMessageHeader header;
	std::string service_auth_token;
	std::string session_id;
	std::string helper_process_id;
	std::uint64_t helper_start_time_ms = 0;
};

struct IpcHandshakeResponse {
	IpcMessageHeader header;
	bool authenticated = false;
	IpcErrorCode error = IpcErrorCode::kNone;
	std::string session_id;
	std::string service_process_id;
};

struct IpcCapabilityAdvertisement {
	IpcMessageHeader header;
	std::string session_id;
	HelperCapabilityFlag available_capabilities = HelperCapabilityFlag::kNone;
	std::vector<std::string> feature_flags;
	std::uint32_t desktop_session_id = 0;
};

struct IpcCapabilityAcknowledgment {
	IpcMessageHeader header;
	bool accepted = false;
	IpcErrorCode error = IpcErrorCode::kNone;
	HelperCapabilityFlag enabled_capabilities = HelperCapabilityFlag::kNone;
};

struct IpcSessionTransfer {
	IpcMessageHeader header;
	std::string session_id;
	std::string transfer_token;
	std::uint64_t expires_at_ms = 0;
};

struct IpcSessionTransferAck {
	IpcMessageHeader header;
	bool accepted = false;
	IpcErrorCode error = IpcErrorCode::kNone;
};

struct IpcKeepalive {
	IpcMessageHeader header;
	std::string session_id;
};

struct IpcKeepaliveAck {
	IpcMessageHeader header;
	std::string session_id;
	std::uint64_t service_uptime_ms = 0;
};

struct IpcShutdown {
	IpcMessageHeader header;
	std::string session_id;
	std::string reason_code;
	bool graceful = true;
};

struct IpcShutdownAck {
	IpcMessageHeader header;
	bool acknowledged = false;
};

struct IpcErrorMessage {
	IpcMessageHeader header;
	IpcErrorCode error_code = IpcErrorCode::kInternalError;
	std::string error_detail;
};

template <typename TMessage>
struct IpcParseResult {
	bool ok = false;
	TMessage message {};
	IpcErrorCode error = IpcErrorCode::kInternalError;
	std::string error_detail;
};

bool validate_ipc_handshake_request(const IpcHandshakeRequest& request, std::string* error = nullptr);
bool validate_ipc_handshake_response(const IpcHandshakeResponse& response, std::string* error = nullptr);
bool validate_ipc_capability_advertisement(const IpcCapabilityAdvertisement& ad, std::string* error = nullptr);
bool validate_ipc_capability_acknowledgment(const IpcCapabilityAcknowledgment& ack, std::string* error = nullptr);
bool validate_ipc_session_transfer(const IpcSessionTransfer& transfer, std::string* error = nullptr);
bool validate_ipc_session_transfer_ack(const IpcSessionTransferAck& ack, std::string* error = nullptr);
bool validate_ipc_keepalive(const IpcKeepalive& keepalive, std::string* error = nullptr);
bool validate_ipc_keepalive_ack(const IpcKeepaliveAck& ack, std::string* error = nullptr);
bool validate_ipc_shutdown(const IpcShutdown& shutdown, std::string* error = nullptr);
bool validate_ipc_shutdown_ack(const IpcShutdownAck& ack, std::string* error = nullptr);
bool validate_ipc_error_message(const IpcErrorMessage& error_msg, std::string* error = nullptr);

std::string serialize_ipc_handshake_request(const IpcHandshakeRequest& request);
std::string serialize_ipc_handshake_response(const IpcHandshakeResponse& response);
std::string serialize_ipc_capability_advertisement(const IpcCapabilityAdvertisement& ad);
std::string serialize_ipc_capability_acknowledgment(const IpcCapabilityAcknowledgment& ack);
std::string serialize_ipc_session_transfer(const IpcSessionTransfer& transfer);
std::string serialize_ipc_session_transfer_ack(const IpcSessionTransferAck& ack);
std::string serialize_ipc_keepalive(const IpcKeepalive& keepalive);
std::string serialize_ipc_keepalive_ack(const IpcKeepaliveAck& ack);
std::string serialize_ipc_shutdown(const IpcShutdown& shutdown);
std::string serialize_ipc_shutdown_ack(const IpcShutdownAck& ack);
std::string serialize_ipc_error_message(const IpcErrorMessage& error_msg);

IpcParseResult<IpcHandshakeRequest> parse_ipc_handshake_request(std::string_view serialized);
IpcParseResult<IpcHandshakeResponse> parse_ipc_handshake_response(std::string_view serialized);
IpcParseResult<IpcCapabilityAdvertisement> parse_ipc_capability_advertisement(std::string_view serialized);
IpcParseResult<IpcCapabilityAcknowledgment> parse_ipc_capability_acknowledgment(std::string_view serialized);
IpcParseResult<IpcSessionTransfer> parse_ipc_session_transfer(std::string_view serialized);
IpcParseResult<IpcSessionTransferAck> parse_ipc_session_transfer_ack(std::string_view serialized);
IpcParseResult<IpcKeepalive> parse_ipc_keepalive(std::string_view serialized);
IpcParseResult<IpcKeepaliveAck> parse_ipc_keepalive_ack(std::string_view serialized);
IpcParseResult<IpcShutdown> parse_ipc_shutdown(std::string_view serialized);
IpcParseResult<IpcShutdownAck> parse_ipc_shutdown_ack(std::string_view serialized);
IpcParseResult<IpcErrorMessage> parse_ipc_error_message(std::string_view serialized);

std::string to_string(IpcMessageType type);
std::string to_string(IpcErrorCode error);
std::string to_string(HelperCapabilityFlag flags);

}
