#include "redclaw/protocol/agent_protocol.h"

#include <boost/json.hpp>
#include <charconv>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {
using namespace redclaw::protocol;

bool text_value(const boost::json::value& value, std::string& out) {
    if (!value.is_string()) return false;
    out.assign(value.as_string().data(), value.as_string().size());
    return true;
}
template <typename T>
bool integer_value(const boost::json::value& value, T& out) {
    const std::string text = value.is_string()
        ? std::string(value.as_string()) : boost::json::serialize(value);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool boolean_value(const boost::json::value& value, bool& out) {
    if (value.is_bool()) { out = value.as_bool(); return true; }
    if (value.is_string() && value.as_string() == "true") { out = true; return true; }
    if (value.is_string() && value.as_string() == "false") { out = false; return true; }
    return false;
}
template <typename E>
bool enum_value(const boost::json::value& value, E& out, int last) {
    if (!value.is_string()) return false;
    for (int i = 0; i <= last; ++i) {
        const auto candidate = static_cast<E>(i);
        if (value.as_string() == to_string(candidate)) { out = candidate; return true; }
    }
    return false;
}
bool from_json(const boost::json::object& json, AgentMessageEnvelopeV1& message) {
    for (const auto& entry : json) {
        const std::string_view key(entry.key().data(), entry.key().size());
        const auto& value = entry.value();
        if (key == "schema_version") { if (!integer_value(value, message.schema_version)) return false; }
        else if (key == "session_epoch") { if (!text_value(value, message.session_epoch)) return false; }
        else if (key == "message_id") { if (!integer_value(value, message.message_id)) return false; }
        else if (key == "sent_at_ms") { if (!integer_value(value, message.sent_at_ms)) return false; }
        else if (key == "task_id") { if (!text_value(value, message.task_id)) return false; }
        else if (key == "request_id") { if (!text_value(value, message.request_id)) return false; }
        else if (key == "supersedes_request_id") { if (!text_value(value, message.supersedes_request_id)) return false; }
        else if (key == "event_sequence") { if (!integer_value(value, message.event_sequence)) return false; }
        else if (key == "message_type") { if (!enum_value(value, message.type, 13)) return false; }
        else if (key == "provider") { if (!enum_value(value, message.provider, 2)) return false; }
        else if (key == "provider_readiness") { if (!enum_value(value, message.provider_readiness, 4)) return false; }
        else if (key == "work_directory_mode") { if (!enum_value(value, message.work_directory_mode, 1)) return false; }
        else if (key == "task_state") { if (!enum_value(value, message.task_state, 9)) return false; }
        else if (key == "approval_decision") { if (!enum_value(value, message.approval_decision, 2)) return false; }
        else if (key == "model") { if (!text_value(value, message.model)) return false; }
        else if (key == "project_id") { if (!text_value(value, message.project_id)) return false; }
        else if (key == "display_name") { if (!text_value(value, message.display_name)) return false; }
        else if (key == "event_kind") { if (!text_value(value, message.event_kind)) return false; }
        else if (key == "error_code") { if (!text_value(value, message.error_code)) return false; }
        else if (key == "evidence_manifest_name") { if (!text_value(value, message.evidence_manifest_name)) return false; }
        else if (key == "evidence_sha256") { if (!text_value(value, message.evidence_sha256)) return false; }
        else if (key == "text") { if (!text_value(value, message.text)) return false; }
        else if (key == "acknowledged_event_sequence") { if (!integer_value(value, message.acknowledged_event_sequence)) return false; }
        else if (key == "available") { if (!boolean_value(value, message.available)) return false; }
        else if (key == "complete") { if (!boolean_value(value, message.complete)) return false; }
        else if (key == "gap") { if (!boolean_value(value, message.gap)) return false; }
        else if (key == "git_repository") { if (!boolean_value(value, message.git_repository)) return false; }
        else if (key == "requires_turn_approval") { if (!boolean_value(value, message.requires_turn_approval)) return false; }
        else if (key == "supports_structured_approval") { if (!boolean_value(value, message.supports_structured_approval)) return false; }
        else return false;
    }
    return validate_agent_message_v1(message);
}
boost::json::object to_json(const AgentMessageEnvelopeV1& message) {
    boost::json::object json;
    json["schema_version"] = std::to_string(message.schema_version);
    json["session_epoch"] = message.session_epoch;
    json["message_id"] = std::to_string(message.message_id);
    json["sent_at_ms"] = std::to_string(message.sent_at_ms);
    json["task_id"] = message.task_id;
    json["request_id"] = message.request_id;
    json["supersedes_request_id"] = message.supersedes_request_id;
    json["event_sequence"] = std::to_string(message.event_sequence);
    json["message_type"] = std::string(to_string(message.type));
    json["provider"] = std::string(to_string(message.provider));
    json["provider_readiness"] = std::string(to_string(message.provider_readiness));
    json["work_directory_mode"] = std::string(to_string(message.work_directory_mode));
    json["task_state"] = std::string(to_string(message.task_state));
    json["approval_decision"] = std::string(to_string(message.approval_decision));
    json["model"] = message.model;
    json["project_id"] = message.project_id;
    json["display_name"] = message.display_name;
    json["event_kind"] = message.event_kind;
    json["error_code"] = message.error_code;
    json["evidence_manifest_name"] = message.evidence_manifest_name;
    json["evidence_sha256"] = message.evidence_sha256;
    json["text"] = message.text;
    json["acknowledged_event_sequence"] = std::to_string(message.acknowledged_event_sequence);
    json["available"] = (message.available ? "true" : "false");
    json["complete"] = (message.complete ? "true" : "false");
    json["gap"] = (message.gap ? "true" : "false");
    json["git_repository"] = (message.git_repository ? "true" : "false");
    json["requires_turn_approval"] = (message.requires_turn_approval ? "true" : "false");
    json["supports_structured_approval"] = (message.supports_structured_approval ? "true" : "false");
    return json;
}
}  // namespace

// Stdio codec only: no network, provider, filesystem access or task execution.
// Instructions travel on stdin, never command-line arguments or diagnostic logs.
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        std::string input;
        char chunk[4096];
        while (std::cin.read(chunk, sizeof(chunk)) || std::cin.gcount() > 0) {
            if (input.size() + static_cast<std::size_t>(std::cin.gcount()) > 256U * 1024U) return 2;
            input.append(chunk, static_cast<std::size_t>(std::cin.gcount()));
        }
        const std::string_view operation(argv[1]);
        if (operation == "--encode-agent") {
            const auto value = boost::json::parse(input);
            redclaw::protocol::AgentMessageEnvelopeV1 message;
            if (!value.is_object() || !from_json(value.as_object(), message)) return 2;
            const auto frame = redclaw::protocol::serialize_local_runtime_agent_frame_v1(message);
            if (frame.empty() || frame.size() > redclaw::protocol::kMaxLocalRuntimeAgentFrameBytes) return 2;
            std::cout << frame;
        } else if (operation == "--decode-agent") {
            const auto parsed = redclaw::protocol::parse_local_runtime_agent_frame_v1(input);
            if (!parsed.ok) return 2;
            std::cout << boost::json::serialize(to_json(parsed.value));
        } else return 2;
        return std::cout.good() ? 0 : 2;
    } catch (...) {
        // Never echo malformed input: it may contain instruction text or secrets.
        std::cerr << "Agent wire codec rejected input";
        return 2;
    }
}
