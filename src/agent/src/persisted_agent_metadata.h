#pragma once

#include <charconv>
#include <limits>
#include <map>

#include <openssl/evp.h>
#include "redclaw/protocol/agent_protocol.h"

namespace redclaw::agent::detail {

// Upgrade-only reader for local completed-task records. Never used by network
// or API parsers. Only metadata is imported; no instruction or task is executed.
inline redclaw::protocol::ParseResult<redclaw::protocol::AgentMessageEnvelopeV1>
parse_completed_agent_metadata(std::string_view line) {
    using namespace redclaw::protocol;
    auto result = parse_local_runtime_agent_frame_v1(line);
    if (result.ok) return result;
    constexpr std::string_view prefix = "RCD-LOCAL-AGENT-V1 ";
    if (line.size() > kMaxLocalRuntimeAgentFrameBytes || !line.starts_with(prefix)) return result;
    const auto encoded = line.substr(prefix.size());
    if (encoded.empty() || encoded.size() % 4 != 0) return result;
    std::string plain(encoded.size() / 4 * 3, '\0');
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(plain.data()),
        reinterpret_cast<const unsigned char*>(encoded.data()), static_cast<int>(encoded.size()));
    if (size < 0) return result;
    std::size_t padding = encoded.ends_with("==") ? 2 : (encoded.ends_with("=") ? 1 : 0);
    if (static_cast<std::size_t>(size) < padding) return result;
    plain.resize(static_cast<std::size_t>(size) - padding);
    constexpr std::string_view header = "RCD-AGENT-V1\n";
    if (!plain.starts_with(header)) return result;
    std::map<std::string, std::string> fields;
    std::string_view remaining(plain);
    remaining.remove_prefix(header.size());
    while (!remaining.empty()) {
        const auto end = remaining.find('\n');
        if (end == std::string_view::npos || fields.size() >= 40) return result;
        const auto field = remaining.substr(0, end);
        remaining.remove_prefix(end + 1);
        const auto equals = field.find('=');
        if (equals == std::string_view::npos) return result;
        std::string value;
        const auto escaped = field.substr(equals + 1);
        for (std::size_t i = 0; i < escaped.size(); ++i) {
            char ch = escaped[i];
            if (ch == '\\') {
                if (++i == escaped.size()) return result;
                ch = escaped[i];
                if (ch == 'n') ch = '\n';
                else if (ch == 'r') ch = '\r';
                else if (ch != '\\' && ch != '=') return result;
            }
            value += ch;
        }
        if (!fields.emplace(std::string(field.substr(0, equals)), std::move(value)).second) return result;
    }
    if (fields["schema_version"] != "1" || fields["message_type"] != "agent_task_snapshot"
        || fields["session_epoch"] != "persisted-agent-metadata" || !fields["text"].empty()) return result;
    AgentMessageEnvelopeV1 message;
    message.type = AgentMessageTypeV1::kTaskSnapshot;
    message.session_epoch = "persisted-agent-metadata";
    message.task_id = fields["task_id"];
    message.project_id = fields["project_id"];
    message.model = fields["model"];
    const auto number = [&](const char* key, std::uint64_t& value) {
        const auto& text = fields[key];
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    };
    if (!number("message_id", message.message_id) || !number("sent_at_ms", message.sent_at_ms)
        || !number("event_sequence", message.event_sequence)
        || message.event_sequence == std::numeric_limits<std::uint64_t>::max()) return result;
    if (fields["provider"] == "codex") message.provider = AgentProviderKindV1::kCodex;
    else if (fields["provider"] == "cursor") message.provider = AgentProviderKindV1::kCursor;
    else return result;
    if (fields["task_state"] == "completed") message.task_state = AgentTaskStateV1::kCompleted;
    else if (fields["task_state"] == "failed") message.task_state = AgentTaskStateV1::kFailed;
    else if (fields["task_state"] == "interrupted") message.task_state = AgentTaskStateV1::kInterrupted;
    else return result;
    if (fields["work_directory_mode"] == "direct_workspace")
        message.work_directory_mode = AgentWorkDirectoryModeV1::kDirectWorkspace;
    else if (fields["work_directory_mode"] != "isolated_worktree") return result;
    if (message.task_id.empty() || !validate_agent_message_v1(message)) return result;
    result.ok = true;
    result.value = std::move(message);
    result.error.clear();
    return result;
}

}  // namespace redclaw::agent::detail
