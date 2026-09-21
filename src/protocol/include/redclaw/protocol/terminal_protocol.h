#pragma once
#include "redclaw/protocol/protocol_module.h"
#include <cstddef>

namespace redclaw::protocol {
inline constexpr std::size_t kMaxTerminalChunkBytes = 16U * 1024U;
enum class TerminalMessageTypeV1 {
    kOpen, kReady, kInput, kOutput, kOutputAck, kResize, kState, kAvailability, kEnd, kEnded,
    kExec, kExecState, kCancel
};
struct TerminalMessageV1 {
    int schema_version = 1;
    TerminalMessageTypeV1 type = TerminalMessageTypeV1::kOpen;
    std::string session_epoch;
    std::string terminal_id;
    std::string client_session_id;
    std::uint64_t sequence = 0;
    std::uint64_t input_generation = 0;
    std::uint64_t input_sequence = 0;
    std::uint32_t columns = 0, rows = 0;
    bool input_enabled = false;
    bool local_input_paused = false; // GUI/runtime availability only; never accepted from the network
    bool exited = false;
    std::string error_code;
    std::string bytes;
    std::uint32_t capability_version = 0;
    std::string operation_id, execution_state;
    std::uint64_t output_position = 0;
    bool prompt_ready = false, powershell_success = false, has_native_exit_code = false;
    std::int32_t last_native_exit_code = 0;
};
[[nodiscard]] bool validate_terminal_message_v1(const TerminalMessageV1& message, std::string* error = nullptr);
[[nodiscard]] std::string serialize_terminal_message_v1(const TerminalMessageV1& message);
[[nodiscard]] ParseResult<TerminalMessageV1> parse_terminal_message_v1(std::string_view frame);
}
