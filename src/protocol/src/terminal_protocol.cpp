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
    if (message.type < TerminalMessageTypeV1::kOpen || message.type > TerminalMessageTypeV1::kCancel) {
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
    const bool execution = message.type == TerminalMessageTypeV1::kExec;
    const bool captured_output = message.type == TerminalMessageTypeV1::kExecState && message.execution_state == "output";
    const bool data = message.type == TerminalMessageTypeV1::kInput || message.type == TerminalMessageTypeV1::kOutput || execution || captured_output;
    if ((data && (message.sequence == 0 || message.bytes.empty() || message.bytes.size() > kMaxTerminalChunkBytes))
        || (!data && !message.bytes.empty())) return reject(error, "terminal_invalid_payload");
    if ((message.type == TerminalMessageTypeV1::kInput || message.type == TerminalMessageTypeV1::kResize || execution || message.type == TerminalMessageTypeV1::kCancel)
        && message.input_generation == 0) return reject(error, "terminal_invalid_input_generation");
    if (message.local_input_paused && message.type != TerminalMessageTypeV1::kAvailability)
        return reject(error, "terminal_invalid_local_status");
    if ((execution || message.type == TerminalMessageTypeV1::kCancel || message.type == TerminalMessageTypeV1::kExecState || !message.operation_id.empty())
        && !token(message.operation_id, 128)) return reject(error, "terminal_invalid_operation");
    if (!message.execution_state.empty() && !token(message.execution_state, 64))
        return reject(error, "terminal_invalid_execution_state");
    if (message.type == TerminalMessageTypeV1::kExecState && message.execution_state != "running"
        && message.execution_state != "output" && message.execution_state != "cancelling"
        && message.execution_state != "succeeded" && message.execution_state != "failed"
        && message.execution_state != "cancelled" && message.execution_state != "unknown" && message.execution_state != "rejected")
        return reject(error, "terminal_invalid_execution_state");
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
    encoded.set_capability_version(message.capability_version);
    encoded.set_operation_id(message.operation_id); encoded.set_execution_state(message.execution_state);
    encoded.set_output_position(message.output_position); encoded.set_prompt_ready(message.prompt_ready);
    encoded.set_powershell_success(message.powershell_success); encoded.set_has_native_exit_code(message.has_native_exit_code);
    encoded.set_last_native_exit_code(message.last_native_exit_code);
    return compress_protobuf(encoded, ProtobufWireKind::kTerminal);
}
ParseResult<TerminalMessageV1> parse_terminal_message_v1(std::string_view frame) {
    ParseResult<TerminalMessageV1> result;
    wire::TerminalMessageV1 encoded;
    if (!decompress_protobuf(frame, ProtobufWireKind::kTerminal, encoded, &result.error)) return result;
    if (encoded.type() > static_cast<std::uint32_t>(TerminalMessageTypeV1::kCancel)) {
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
    message.capability_version = encoded.capability_version();
    message.operation_id = encoded.operation_id(); message.execution_state = encoded.execution_state();
    message.output_position = encoded.output_position(); message.prompt_ready = encoded.prompt_ready();
    message.powershell_success = encoded.powershell_success(); message.has_native_exit_code = encoded.has_native_exit_code();
    message.last_native_exit_code = encoded.last_native_exit_code();
    result.ok = validate_terminal_message_v1(message, &result.error);
    return result;
}
}
