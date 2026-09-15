#include "redclaw/protocol/terminal_protocol.h"
#include "redclaw/protocol/compressed_protobuf.h"
#include "redclaw_wire.pb.h"

namespace redclaw::protocol {
namespace {
bool token(std::string_view text, std::size_t limit) {
    if (text.empty() || text.size() > limit) return false;
    for (const auto ch : text) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
            || ch == '-' || ch == '_' || ch == '.' || ch == ':')) return false;
    }
    return true;
}
bool reject(std::string* error, const char* code) { if (error) *error = code; return false; }
}
bool validate_terminal_message_v1(const TerminalMessageV1& message, std::string* error) {
    if (message.schema_version != 1) return reject(error, "protocol_version_incompatible");
    if (!token(message.session_epoch, 128)) return reject(error, "terminal_invalid_epoch");
    if (message.type < TerminalMessageTypeV1::kOpen || message.type > TerminalMessageTypeV1::kEnded) {
        return reject(error, "terminal_unknown_message");
    }
    if ((message.type != TerminalMessageTypeV1::kOpen && message.type != TerminalMessageTypeV1::kAvailability
            && message.type != TerminalMessageTypeV1::kEnd && message.type != TerminalMessageTypeV1::kEnded
            || !message.terminal_id.empty())
        && !token(message.terminal_id, 128)) return reject(error, "terminal_invalid_identity");
    if (!message.error_code.empty() && !token(message.error_code, 128)) return reject(error, "terminal_invalid_error");
    if ((message.type == TerminalMessageTypeV1::kOpen || message.type == TerminalMessageTypeV1::kEnd
            || message.type == TerminalMessageTypeV1::kEnded || !message.client_session_id.empty())
        && !token(message.client_session_id, 128)) return reject(error, "terminal_invalid_client_session");
    const bool sized = message.type == TerminalMessageTypeV1::kOpen || message.type == TerminalMessageTypeV1::kResize;
    if (sized && (message.columns < 2 || message.columns > 32767 || message.rows < 2 || message.rows > 32767)) {
        return reject(error, "terminal_invalid_size");
    }
    const bool data = message.type == TerminalMessageTypeV1::kInput || message.type == TerminalMessageTypeV1::kOutput;
    if ((data && (message.sequence == 0 || message.bytes.empty() || message.bytes.size() > kMaxTerminalChunkBytes))
        || (!data && !message.bytes.empty())) return reject(error, "terminal_invalid_payload");
    if ((message.type == TerminalMessageTypeV1::kInput || message.type == TerminalMessageTypeV1::kResize)
        && message.input_generation == 0) return reject(error, "terminal_invalid_input_generation");
    if (message.local_input_paused && message.type != TerminalMessageTypeV1::kAvailability)
        return reject(error, "terminal_invalid_local_status");
    if (error) error->clear();
    return true;
}
std::string serialize_terminal_message_v1(const TerminalMessageV1& message) {
    if (!validate_terminal_message_v1(message)) return {};
    wire::TerminalMessageV1 encoded;
    encoded.set_schema_version(message.schema_version);
    encoded.set_type(static_cast<std::uint32_t>(message.type));
    encoded.set_session_epoch(message.session_epoch);
    encoded.set_terminal_id(message.terminal_id);
    encoded.set_client_session_id(message.client_session_id);
    encoded.set_sequence(message.sequence);
    encoded.set_input_generation(message.input_generation); encoded.set_input_sequence(message.input_sequence);
    encoded.set_columns(message.columns); encoded.set_rows(message.rows);
    encoded.set_input_enabled(message.input_enabled); encoded.set_exited(message.exited);
    encoded.set_local_input_paused(message.local_input_paused);
    encoded.set_error_code(message.error_code); encoded.set_data(message.bytes);
    return compress_protobuf(encoded, ProtobufWireKind::kTerminal);
}
ParseResult<TerminalMessageV1> parse_terminal_message_v1(std::string_view frame) {
    ParseResult<TerminalMessageV1> result;
    wire::TerminalMessageV1 encoded;
    if (!decompress_protobuf(frame, ProtobufWireKind::kTerminal, encoded, &result.error)) return result;
    if (encoded.type() > static_cast<std::uint32_t>(TerminalMessageTypeV1::kEnded)) {
        result.error = "terminal_unknown_message"; return result;
    }
    auto& message = result.value;
    message.schema_version = encoded.schema_version();
    message.type = static_cast<TerminalMessageTypeV1>(encoded.type());
    message.session_epoch = encoded.session_epoch(); message.terminal_id = encoded.terminal_id();
    message.client_session_id = encoded.client_session_id();
    message.sequence = encoded.sequence(); message.columns = encoded.columns(); message.rows = encoded.rows();
    message.input_generation = encoded.input_generation(); message.input_sequence = encoded.input_sequence();
    message.input_enabled = encoded.input_enabled(); message.exited = encoded.exited();
    message.local_input_paused = encoded.local_input_paused();
    message.error_code = encoded.error_code(); message.bytes = encoded.data();
    result.ok = validate_terminal_message_v1(message, &result.error);
    return result;
}
}
