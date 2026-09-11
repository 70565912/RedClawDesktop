#include "redclaw/service/ipc_channel_contracts.h"

#include <sstream>
#include <cctype>
#include <charconv>
#include <unordered_map>

namespace redclaw::service {

namespace {

using FieldMap = std::unordered_map<std::string, std::string>;

constexpr std::string_view kHeaderIpcHandshakeRequest = "RCD-IPC-HANDSHAKE-REQ-V1";
constexpr std::string_view kHeaderIpcHandshakeResponse = "RCD-IPC-HANDSHAKE-RESP-V1";
constexpr std::string_view kHeaderIpcCapabilityAd = "RCD-IPC-CAPABILITY-AD-V1";
constexpr std::string_view kHeaderIpcCapabilityAck = "RCD-IPC-CAPABILITY-ACK-V1";
constexpr std::string_view kHeaderIpcSessionTransfer = "RCD-IPC-SESSION-TRANSFER-V1";
constexpr std::string_view kHeaderIpcSessionTransferAck = "RCD-IPC-SESSION-TRANSFER-ACK-V1";
constexpr std::string_view kHeaderIpcKeepalive = "RCD-IPC-KEEPALIVE-V1";
constexpr std::string_view kHeaderIpcKeepaliveAck = "RCD-IPC-KEEPALIVE-ACK-V1";
constexpr std::string_view kHeaderIpcShutdown = "RCD-IPC-SHUTDOWN-V1";
constexpr std::string_view kHeaderIpcShutdownAck = "RCD-IPC-SHUTDOWN-ACK-V1";
constexpr std::string_view kHeaderIpcError = "RCD-IPC-ERROR-V1";

std::string trim(std::string_view value) {
	std::size_t start = 0;
	std::size_t end = value.size();

	while (start < end && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
		++start;
	}
	while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
		--end;
	}

	return std::string(value.substr(start, end - start));
}

std::string escape(std::string_view value) {
	std::string out;
	out.reserve(value.size());

	for (char ch : value) {
		if (ch == '\\' || ch == '=' || ch == '\n') {
			out.push_back('\\');
			if (ch == '\n') {
				out.push_back('n');
			} else {
				out.push_back(ch);
			}
		} else {
			out.push_back(ch);
		}
	}
	return out;
}

std::string unescape(std::string_view value) {
	std::string out;
	out.reserve(value.size());

	for (std::size_t i = 0; i < value.size(); ++i) {
		if (value[i] == '\\' && i + 1 < value.size()) {
			const char next = value[i + 1];
			if (next == 'n') {
				out.push_back('\n');
			} else if (next == '\\' || next == '=') {
				out.push_back(next);
			} else {
				out.push_back(value[i]);
				out.push_back(next);
			}
			++i;
		} else {
			out.push_back(value[i]);
		}
	}
	return out;
}

FieldMap parse_fields(std::string_view body) {
	FieldMap fields;
	std::string body_str(body);
	std::istringstream iss(body_str);
	std::string line;

	while (std::getline(iss, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}

		const std::size_t eq_pos = line.find('=');
		if (eq_pos == std::string::npos) {
			continue;
		}

		std::string key = trim(line.substr(0, eq_pos));
		std::string value = trim(line.substr(eq_pos + 1));
		
		// For repeated keys, append with delimiter
		if (fields.find(key) != fields.end()) {
			fields[key] += "\n" + unescape(value);
		} else {
			fields[key] = unescape(value);
		}
	}
	return fields;
}

std::string serialize_header(const IpcMessageHeader& header) {
	std::ostringstream oss;
	oss << "schema_version=" << header.schema_version << "\n";
	oss << "message_type=" << static_cast<int>(header.message_type) << "\n";
	oss << "sequence_number=" << header.sequence_number << "\n";
	oss << "timestamp_ms=" << header.timestamp_ms << "\n";
	return oss.str();
}

bool parse_header(const FieldMap& fields, IpcMessageHeader& header, std::string* error) {
	const auto schema_it = fields.find("schema_version");
	if (schema_it == fields.end()) {
		if (error) *error = "missing schema_version";
		return false;
	}

	int schema_version = 0;
	const auto [ptr, ec] = std::from_chars(
		schema_it->second.data(),
		schema_it->second.data() + schema_it->second.size(),
		schema_version);
	if (ec != std::errc{}) {
		if (error) *error = "invalid schema_version";
		return false;
	}

	header.schema_version = schema_version;

	const auto type_it = fields.find("message_type");
	if (type_it != fields.end()) {
		int msg_type = 0;
		const auto [type_ptr, type_ec] = std::from_chars(
			type_it->second.data(),
			type_it->second.data() + type_it->second.size(),
			msg_type);
		if (type_ec == std::errc{}) {
			header.message_type = static_cast<IpcMessageType>(msg_type);
		}
	}

	const auto seq_it = fields.find("sequence_number");
	if (seq_it != fields.end()) {
		std::uint32_t seq = 0;
		const auto [seq_ptr, seq_ec] = std::from_chars(
			seq_it->second.data(),
			seq_it->second.data() + seq_it->second.size(),
			seq);
		if (seq_ec == std::errc{}) {
			header.sequence_number = seq;
		}
	}

	const auto ts_it = fields.find("timestamp_ms");
	if (ts_it != fields.end()) {
		std::uint64_t ts = 0;
		const auto [ts_ptr, ts_ec] = std::from_chars(
			ts_it->second.data(),
			ts_it->second.data() + ts_it->second.size(),
			ts);
		if (ts_ec == std::errc{}) {
			header.timestamp_ms = ts;
		}
	}

	return true;
}

std::string get_field(const FieldMap& fields, const std::string& key) {
	const auto it = fields.find(key);
	return it != fields.end() ? it->second : "";
}

std::uint32_t get_uint32_field(const FieldMap& fields, const std::string& key, std::uint32_t default_value = 0) {
	const auto it = fields.find(key);
	if (it == fields.end()) {
		return default_value;
	}

	std::uint32_t value = 0;
	const auto [ptr, ec] = std::from_chars(
		it->second.data(),
		it->second.data() + it->second.size(),
		value);
	return ec == std::errc{} ? value : default_value;
}

std::uint64_t get_uint64_field(const FieldMap& fields, const std::string& key, std::uint64_t default_value = 0) {
	const auto it = fields.find(key);
	if (it == fields.end()) {
		return default_value;
	}

	std::uint64_t value = 0;
	const auto [ptr, ec] = std::from_chars(
		it->second.data(),
		it->second.data() + it->second.size(),
		value);
	return ec == std::errc{} ? value : default_value;
}

bool get_bool_field(const FieldMap& fields, const std::string& key, bool default_value = false) {
	const auto it = fields.find(key);
	if (it == fields.end()) {
		return default_value;
	}
	return it->second == "true" || it->second == "1";
}

}  // namespace

// Validation functions
bool validate_ipc_handshake_request(const IpcHandshakeRequest& request, std::string* error) {
	if (request.header.schema_version != kIpcSchemaVersionV1) {
		if (error) *error = "invalid schema version";
		return false;
	}
	if (request.service_auth_token.empty()) {
		if (error) *error = "missing auth token";
		return false;
	}
	if (request.session_id.empty()) {
		if (error) *error = "missing session_id";
		return false;
	}
	if (request.helper_process_id.empty()) {
		if (error) *error = "missing helper_process_id";
		return false;
	}
	return true;
}

bool validate_ipc_handshake_response(const IpcHandshakeResponse& response, std::string* error) {
	if (response.header.schema_version != kIpcSchemaVersionV1) {
		if (error) *error = "invalid schema version";
		return false;
	}
	return true;
}

bool validate_ipc_capability_advertisement(const IpcCapabilityAdvertisement& ad, std::string* error) {
	if (ad.header.schema_version != kIpcSchemaVersionV1) {
		if (error) *error = "invalid schema version";
		return false;
	}
	if (ad.session_id.empty()) {
		if (error) *error = "missing session_id";
		return false;
	}
	return true;
}

bool validate_ipc_capability_acknowledgment(const IpcCapabilityAcknowledgment& ack, std::string* error) {
	if (ack.header.schema_version != kIpcSchemaVersionV1) {
		if (error) *error = "invalid schema version";
		return false;
	}
	return true;
}

bool validate_ipc_session_transfer(const IpcSessionTransfer& transfer, std::string* error) {
	if (transfer.header.schema_version != kIpcSchemaVersionV1) {
		if (error) *error = "invalid schema version";
		return false;
	}
	if (transfer.session_id.empty()) {
		if (error) *error = "missing session_id";
		return false;
	}
	if (transfer.transfer_token.empty()) {
		if (error) *error = "missing transfer_token";
		return false;
	}
	return true;
}

bool validate_ipc_session_transfer_ack(const IpcSessionTransferAck& ack, std::string* error) {
	if (ack.header.schema_version != kIpcSchemaVersionV1) {
		if (error) *error = "invalid schema version";
		return false;
	}
	return true;
}

bool validate_ipc_keepalive(const IpcKeepalive& keepalive, std::string* error) {
	if (keepalive.header.schema_version != kIpcSchemaVersionV1) {
		if (error) *error = "invalid schema version";
		return false;
	}
	if (keepalive.session_id.empty()) {
		if (error) *error = "missing session_id";
		return false;
	}
	return true;
}

bool validate_ipc_keepalive_ack(const IpcKeepaliveAck& ack, std::string* error) {
	if (ack.header.schema_version != kIpcSchemaVersionV1) {
		if (error) *error = "invalid schema version";
		return false;
	}
	if (ack.session_id.empty()) {
		if (error) *error = "missing session_id";
		return false;
	}
	return true;
}

bool validate_ipc_shutdown(const IpcShutdown& shutdown, std::string* error) {
	if (shutdown.header.schema_version != kIpcSchemaVersionV1) {
		if (error) *error = "invalid schema version";
		return false;
	}
	if (shutdown.session_id.empty()) {
		if (error) *error = "missing session_id";
		return false;
	}
	return true;
}

bool validate_ipc_shutdown_ack(const IpcShutdownAck& ack, std::string* error) {
	if (ack.header.schema_version != kIpcSchemaVersionV1) {
		if (error) *error = "invalid schema version";
		return false;
	}
	return true;
}

bool validate_ipc_error_message(const IpcErrorMessage& error_msg, std::string* error) {
	if (error_msg.header.schema_version != kIpcSchemaVersionV1) {
		if (error) *error = "invalid schema version";
		return false;
	}
	return true;
}

// Serialization functions
std::string serialize_ipc_handshake_request(const IpcHandshakeRequest& request) {
	std::ostringstream oss;
	oss << kHeaderIpcHandshakeRequest << "\n";
	oss << serialize_header(request.header);
	oss << "service_auth_token=" << escape(request.service_auth_token) << "\n";
	oss << "session_id=" << escape(request.session_id) << "\n";
	oss << "helper_process_id=" << escape(request.helper_process_id) << "\n";
	oss << "helper_start_time_ms=" << request.helper_start_time_ms << "\n";
	return oss.str();
}

std::string serialize_ipc_handshake_response(const IpcHandshakeResponse& response) {
	std::ostringstream oss;
	oss << kHeaderIpcHandshakeResponse << "\n";
	oss << serialize_header(response.header);
	oss << "authenticated=" << (response.authenticated ? "true" : "false") << "\n";
	oss << "error=" << static_cast<int>(response.error) << "\n";
	oss << "session_id=" << escape(response.session_id) << "\n";
	oss << "service_process_id=" << escape(response.service_process_id) << "\n";
	return oss.str();
}

std::string serialize_ipc_capability_advertisement(const IpcCapabilityAdvertisement& ad) {
	std::ostringstream oss;
	oss << kHeaderIpcCapabilityAd << "\n";
	oss << serialize_header(ad.header);
	oss << "session_id=" << escape(ad.session_id) << "\n";
	oss << "available_capabilities=" << static_cast<std::uint32_t>(ad.available_capabilities) << "\n";
	oss << "desktop_session_id=" << ad.desktop_session_id << "\n";
	for (const auto& flag : ad.feature_flags) {
		oss << "feature_flag=" << escape(flag) << "\n";
	}
	return oss.str();
}

std::string serialize_ipc_capability_acknowledgment(const IpcCapabilityAcknowledgment& ack) {
	std::ostringstream oss;
	oss << kHeaderIpcCapabilityAck << "\n";
	oss << serialize_header(ack.header);
	oss << "accepted=" << (ack.accepted ? "true" : "false") << "\n";
	oss << "error=" << static_cast<int>(ack.error) << "\n";
	oss << "enabled_capabilities=" << static_cast<std::uint32_t>(ack.enabled_capabilities) << "\n";
	return oss.str();
}

std::string serialize_ipc_session_transfer(const IpcSessionTransfer& transfer) {
	std::ostringstream oss;
	oss << kHeaderIpcSessionTransfer << "\n";
	oss << serialize_header(transfer.header);
	oss << "session_id=" << escape(transfer.session_id) << "\n";
	oss << "transfer_token=" << escape(transfer.transfer_token) << "\n";
	oss << "expires_at_ms=" << transfer.expires_at_ms << "\n";
	return oss.str();
}

std::string serialize_ipc_session_transfer_ack(const IpcSessionTransferAck& ack) {
	std::ostringstream oss;
	oss << kHeaderIpcSessionTransferAck << "\n";
	oss << serialize_header(ack.header);
	oss << "accepted=" << (ack.accepted ? "true" : "false") << "\n";
	oss << "error=" << static_cast<int>(ack.error) << "\n";
	return oss.str();
}

std::string serialize_ipc_keepalive(const IpcKeepalive& keepalive) {
	std::ostringstream oss;
	oss << kHeaderIpcKeepalive << "\n";
	oss << serialize_header(keepalive.header);
	oss << "session_id=" << escape(keepalive.session_id) << "\n";
	return oss.str();
}

std::string serialize_ipc_keepalive_ack(const IpcKeepaliveAck& ack) {
	std::ostringstream oss;
	oss << kHeaderIpcKeepaliveAck << "\n";
	oss << serialize_header(ack.header);
	oss << "session_id=" << escape(ack.session_id) << "\n";
	oss << "service_uptime_ms=" << ack.service_uptime_ms << "\n";
	return oss.str();
}

std::string serialize_ipc_shutdown(const IpcShutdown& shutdown) {
	std::ostringstream oss;
	oss << kHeaderIpcShutdown << "\n";
	oss << serialize_header(shutdown.header);
	oss << "session_id=" << escape(shutdown.session_id) << "\n";
	oss << "reason_code=" << escape(shutdown.reason_code) << "\n";
	oss << "graceful=" << (shutdown.graceful ? "true" : "false") << "\n";
	return oss.str();
}

std::string serialize_ipc_shutdown_ack(const IpcShutdownAck& ack) {
	std::ostringstream oss;
	oss << kHeaderIpcShutdownAck << "\n";
	oss << serialize_header(ack.header);
	oss << "acknowledged=" << (ack.acknowledged ? "true" : "false") << "\n";
	return oss.str();
}

std::string serialize_ipc_error_message(const IpcErrorMessage& error_msg) {
	std::ostringstream oss;
	oss << kHeaderIpcError << "\n";
	oss << serialize_header(error_msg.header);
	oss << "error_code=" << static_cast<int>(error_msg.error_code) << "\n";
	oss << "error_detail=" << escape(error_msg.error_detail) << "\n";
	return oss.str();
}

// Parse functions
IpcParseResult<IpcHandshakeRequest> parse_ipc_handshake_request(std::string_view serialized) {
	IpcParseResult<IpcHandshakeRequest> result;

	const std::size_t first_newline = serialized.find('\n');
	if (first_newline == std::string_view::npos) {
		result.error_detail = "missing header";
		return result;
	}

	const std::string_view header_line = serialized.substr(0, first_newline);
	if (header_line != kHeaderIpcHandshakeRequest) {
		result.error_detail = "invalid header: expected " + std::string(kHeaderIpcHandshakeRequest);
		return result;
	}

	const std::string_view body = serialized.substr(first_newline + 1);
	FieldMap fields = parse_fields(body);

	if (!parse_header(fields, result.message.header, &result.error_detail)) {
		return result;
	}

	result.message.header.message_type = IpcMessageType::kHandshakeRequest;
	result.message.service_auth_token = get_field(fields, "service_auth_token");
	result.message.session_id = get_field(fields, "session_id");
	result.message.helper_process_id = get_field(fields, "helper_process_id");
	result.message.helper_start_time_ms = get_uint64_field(fields, "helper_start_time_ms");

	if (!validate_ipc_handshake_request(result.message, &result.error_detail)) {
		return result;
	}

	result.ok = true;
	result.error = IpcErrorCode::kNone;
	return result;
}

IpcParseResult<IpcHandshakeResponse> parse_ipc_handshake_response(std::string_view serialized) {
	IpcParseResult<IpcHandshakeResponse> result;

	const std::size_t first_newline = serialized.find('\n');
	if (first_newline == std::string_view::npos) {
		result.error_detail = "missing header";
		return result;
	}

	const std::string_view header_line = serialized.substr(0, first_newline);
	if (header_line != kHeaderIpcHandshakeResponse) {
		result.error_detail = "invalid header";
		return result;
	}

	const std::string_view body = serialized.substr(first_newline + 1);
	FieldMap fields = parse_fields(body);

	if (!parse_header(fields, result.message.header, &result.error_detail)) {
		return result;
	}

	result.message.header.message_type = IpcMessageType::kHandshakeResponse;
	result.message.authenticated = get_bool_field(fields, "authenticated", false);
	result.message.error = static_cast<IpcErrorCode>(get_uint32_field(fields, "error"));
	result.message.session_id = get_field(fields, "session_id");
	result.message.service_process_id = get_field(fields, "service_process_id");

	result.ok = true;
	result.error = IpcErrorCode::kNone;
	return result;
}

IpcParseResult<IpcCapabilityAdvertisement> parse_ipc_capability_advertisement(std::string_view serialized) {
	IpcParseResult<IpcCapabilityAdvertisement> result;

	const std::size_t first_newline = serialized.find('\n');
	if (first_newline == std::string_view::npos) {
		result.error_detail = "missing header";
		return result;
	}

	const std::string_view header_line = serialized.substr(0, first_newline);
	if (header_line != kHeaderIpcCapabilityAd) {
		result.error_detail = "invalid header";
		return result;
	}

	const std::string_view body = serialized.substr(first_newline + 1);
	FieldMap fields = parse_fields(body);

	if (!parse_header(fields, result.message.header, &result.error_detail)) {
		return result;
	}

	result.message.header.message_type = IpcMessageType::kCapabilityAdvertisement;
	result.message.session_id = get_field(fields, "session_id");
	result.message.available_capabilities = static_cast<HelperCapabilityFlag>(
		get_uint32_field(fields, "available_capabilities"));
	result.message.desktop_session_id = get_uint32_field(fields, "desktop_session_id");

	// Collect all feature_flag entries (they're joined with \n)
	const std::string feature_flags_str = get_field(fields, "feature_flag");
	if (!feature_flags_str.empty()) {
		std::istringstream iss(feature_flags_str);
		std::string flag;
		while (std::getline(iss, flag)) {
			if (!flag.empty()) {
				result.message.feature_flags.push_back(flag);
			}
		}
	}

	if (!validate_ipc_capability_advertisement(result.message, &result.error_detail)) {
		return result;
	}

	result.ok = true;
	result.error = IpcErrorCode::kNone;
	return result;
}

IpcParseResult<IpcCapabilityAcknowledgment> parse_ipc_capability_acknowledgment(std::string_view serialized) {
	IpcParseResult<IpcCapabilityAcknowledgment> result;

	const std::size_t first_newline = serialized.find('\n');
	if (first_newline == std::string_view::npos) {
		result.error_detail = "missing header";
		return result;
	}

	const std::string_view header_line = serialized.substr(0, first_newline);
	if (header_line != kHeaderIpcCapabilityAck) {
		result.error_detail = "invalid header";
		return result;
	}

	const std::string_view body = serialized.substr(first_newline + 1);
	FieldMap fields = parse_fields(body);

	if (!parse_header(fields, result.message.header, &result.error_detail)) {
		return result;
	}

	result.message.header.message_type = IpcMessageType::kCapabilityAcknowledgment;
	result.message.accepted = get_bool_field(fields, "accepted", false);
	result.message.error = static_cast<IpcErrorCode>(get_uint32_field(fields, "error"));
	result.message.enabled_capabilities = static_cast<HelperCapabilityFlag>(
		get_uint32_field(fields, "enabled_capabilities"));

	result.ok = true;
	result.error = IpcErrorCode::kNone;
	return result;
}

IpcParseResult<IpcSessionTransfer> parse_ipc_session_transfer(std::string_view serialized) {
	IpcParseResult<IpcSessionTransfer> result;

	const std::size_t first_newline = serialized.find('\n');
	if (first_newline == std::string_view::npos) {
		result.error_detail = "missing header";
		return result;
	}

	const std::string_view header_line = serialized.substr(0, first_newline);
	if (header_line != kHeaderIpcSessionTransfer) {
		result.error_detail = "invalid header";
		return result;
	}

	const std::string_view body = serialized.substr(first_newline + 1);
	FieldMap fields = parse_fields(body);

	if (!parse_header(fields, result.message.header, &result.error_detail)) {
		return result;
	}

	result.message.header.message_type = IpcMessageType::kSessionTransfer;
	result.message.session_id = get_field(fields, "session_id");
	result.message.transfer_token = get_field(fields, "transfer_token");
	result.message.expires_at_ms = get_uint64_field(fields, "expires_at_ms");

	if (!validate_ipc_session_transfer(result.message, &result.error_detail)) {
		return result;
	}

	result.ok = true;
	result.error = IpcErrorCode::kNone;
	return result;
}

IpcParseResult<IpcSessionTransferAck> parse_ipc_session_transfer_ack(std::string_view serialized) {
	IpcParseResult<IpcSessionTransferAck> result;

	const std::size_t first_newline = serialized.find('\n');
	if (first_newline == std::string_view::npos) {
		result.error_detail = "missing header";
		return result;
	}

	const std::string_view header_line = serialized.substr(0, first_newline);
	if (header_line != kHeaderIpcSessionTransferAck) {
		result.error_detail = "invalid header";
		return result;
	}

	const std::string_view body = serialized.substr(first_newline + 1);
	FieldMap fields = parse_fields(body);

	if (!parse_header(fields, result.message.header, &result.error_detail)) {
		return result;
	}

	result.message.header.message_type = IpcMessageType::kSessionTransferAck;
	result.message.accepted = get_bool_field(fields, "accepted", false);
	result.message.error = static_cast<IpcErrorCode>(get_uint32_field(fields, "error"));

	result.ok = true;
	result.error = IpcErrorCode::kNone;
	return result;
}

IpcParseResult<IpcKeepalive> parse_ipc_keepalive(std::string_view serialized) {
	IpcParseResult<IpcKeepalive> result;

	const std::size_t first_newline = serialized.find('\n');
	if (first_newline == std::string_view::npos) {
		result.error_detail = "missing header";
		return result;
	}

	const std::string_view header_line = serialized.substr(0, first_newline);
	if (header_line != kHeaderIpcKeepalive) {
		result.error_detail = "invalid header";
		return result;
	}

	const std::string_view body = serialized.substr(first_newline + 1);
	FieldMap fields = parse_fields(body);

	if (!parse_header(fields, result.message.header, &result.error_detail)) {
		return result;
	}

	result.message.header.message_type = IpcMessageType::kKeepalive;
	result.message.session_id = get_field(fields, "session_id");

	if (!validate_ipc_keepalive(result.message, &result.error_detail)) {
		return result;
	}

	result.ok = true;
	result.error = IpcErrorCode::kNone;
	return result;
}

IpcParseResult<IpcKeepaliveAck> parse_ipc_keepalive_ack(std::string_view serialized) {
	IpcParseResult<IpcKeepaliveAck> result;

	const std::size_t first_newline = serialized.find('\n');
	if (first_newline == std::string_view::npos) {
		result.error_detail = "missing header";
		return result;
	}

	const std::string_view header_line = serialized.substr(0, first_newline);
	if (header_line != kHeaderIpcKeepaliveAck) {
		result.error_detail = "invalid header";
		return result;
	}

	const std::string_view body = serialized.substr(first_newline + 1);
	FieldMap fields = parse_fields(body);

	if (!parse_header(fields, result.message.header, &result.error_detail)) {
		return result;
	}

	result.message.header.message_type = IpcMessageType::kKeepaliveAck;
	result.message.session_id = get_field(fields, "session_id");
	result.message.service_uptime_ms = get_uint64_field(fields, "service_uptime_ms");

	if (!validate_ipc_keepalive_ack(result.message, &result.error_detail)) {
		return result;
	}

	result.ok = true;
	result.error = IpcErrorCode::kNone;
	return result;
}

IpcParseResult<IpcShutdown> parse_ipc_shutdown(std::string_view serialized) {
	IpcParseResult<IpcShutdown> result;

	const std::size_t first_newline = serialized.find('\n');
	if (first_newline == std::string_view::npos) {
		result.error_detail = "missing header";
		return result;
	}

	const std::string_view header_line = serialized.substr(0, first_newline);
	if (header_line != kHeaderIpcShutdown) {
		result.error_detail = "invalid header";
		return result;
	}

	const std::string_view body = serialized.substr(first_newline + 1);
	FieldMap fields = parse_fields(body);

	if (!parse_header(fields, result.message.header, &result.error_detail)) {
		return result;
	}

	result.message.header.message_type = IpcMessageType::kShutdown;
	result.message.session_id = get_field(fields, "session_id");
	result.message.reason_code = get_field(fields, "reason_code");
	result.message.graceful = get_bool_field(fields, "graceful", true);

	if (!validate_ipc_shutdown(result.message, &result.error_detail)) {
		return result;
	}

	result.ok = true;
	result.error = IpcErrorCode::kNone;
	return result;
}

IpcParseResult<IpcShutdownAck> parse_ipc_shutdown_ack(std::string_view serialized) {
	IpcParseResult<IpcShutdownAck> result;

	const std::size_t first_newline = serialized.find('\n');
	if (first_newline == std::string_view::npos) {
		result.error_detail = "missing header";
		return result;
	}

	const std::string_view header_line = serialized.substr(0, first_newline);
	if (header_line != kHeaderIpcShutdownAck) {
		result.error_detail = "invalid header";
		return result;
	}

	const std::string_view body = serialized.substr(first_newline + 1);
	FieldMap fields = parse_fields(body);

	if (!parse_header(fields, result.message.header, &result.error_detail)) {
		return result;
	}

	result.message.header.message_type = IpcMessageType::kShutdownAck;
	result.message.acknowledged = get_bool_field(fields, "acknowledged", false);

	result.ok = true;
	result.error = IpcErrorCode::kNone;
	return result;
}

IpcParseResult<IpcErrorMessage> parse_ipc_error_message(std::string_view serialized) {
	IpcParseResult<IpcErrorMessage> result;

	const std::size_t first_newline = serialized.find('\n');
	if (first_newline == std::string_view::npos) {
		result.error_detail = "missing header";
		return result;
	}

	const std::string_view header_line = serialized.substr(0, first_newline);
	if (header_line != kHeaderIpcError) {
		result.error_detail = "invalid header";
		return result;
	}

	const std::string_view body = serialized.substr(first_newline + 1);
	FieldMap fields = parse_fields(body);

	if (!parse_header(fields, result.message.header, &result.error_detail)) {
		return result;
	}

	result.message.header.message_type = IpcMessageType::kError;
	result.message.error_code = static_cast<IpcErrorCode>(get_uint32_field(fields, "error_code"));
	result.message.error_detail = get_field(fields, "error_detail");

	result.ok = true;
	result.error = IpcErrorCode::kNone;
	return result;
}

// to_string helper functions
std::string to_string(IpcMessageType type) {
	switch (type) {
		case IpcMessageType::kHandshakeRequest: return "HandshakeRequest";
		case IpcMessageType::kHandshakeResponse: return "HandshakeResponse";
		case IpcMessageType::kCapabilityAdvertisement: return "CapabilityAdvertisement";
		case IpcMessageType::kCapabilityAcknowledgment: return "CapabilityAcknowledgment";
		case IpcMessageType::kSessionTransfer: return "SessionTransfer";
		case IpcMessageType::kSessionTransferAck: return "SessionTransferAck";
		case IpcMessageType::kKeepalive: return "Keepalive";
		case IpcMessageType::kKeepaliveAck: return "KeepaliveAck";
		case IpcMessageType::kShutdown: return "Shutdown";
		case IpcMessageType::kShutdownAck: return "ShutdownAck";
		case IpcMessageType::kError: return "Error";
		default: return "Unknown";
	}
}

std::string to_string(IpcErrorCode error) {
	switch (error) {
		case IpcErrorCode::kNone: return "None";
		case IpcErrorCode::kInvalidSchema: return "InvalidSchema";
		case IpcErrorCode::kAuthenticationFailed: return "AuthenticationFailed";
		case IpcErrorCode::kInvalidToken: return "InvalidToken";
		case IpcErrorCode::kSessionMismatch: return "SessionMismatch";
		case IpcErrorCode::kCapabilityRejected: return "CapabilityRejected";
		case IpcErrorCode::kTimeout: return "Timeout";
		case IpcErrorCode::kProtocolViolation: return "ProtocolViolation";
		case IpcErrorCode::kInternalError: return "InternalError";
		default: return "Unknown";
	}
}

std::string to_string(HelperCapabilityFlag flags) {
	std::ostringstream oss;
	bool first = true;

	const auto append_flag = [&](HelperCapabilityFlag flag, const char* name) {
		if (has_capability(flags, flag)) {
			if (!first) oss << "|";
			oss << name;
			first = false;
		}
	};

	append_flag(HelperCapabilityFlag::kUserDesktopCapture, "UserDesktopCapture");
	append_flag(HelperCapabilityFlag::kUserInputInjection, "UserInputInjection");
	append_flag(HelperCapabilityFlag::kClipboardSync, "ClipboardSync");
	append_flag(HelperCapabilityFlag::kFileTransfer, "FileTransfer");
	append_flag(HelperCapabilityFlag::kNotifications, "Notifications");

	if (first) {
		return "None";
	}
	return oss.str();
}

}  // namespace redclaw::service
