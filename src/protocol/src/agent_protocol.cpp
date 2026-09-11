#include "redclaw/protocol/agent_protocol.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace redclaw::protocol {
namespace {

constexpr std::string_view kLocalFramePrefix = "RCD-LOCAL-AGENT-V1 ";
constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void assign_error(std::string value, std::string* error) {
    if (error != nullptr) {
        *error = std::move(value);
    }
}

bool valid_token(std::string_view value, std::size_t max_size, bool allow_empty = false) {
    if ((!allow_empty && value.empty()) || value.size() > max_size) {
        return false;
    }
    for (const char ch : value) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) == 0 && ch != '-' && ch != '_' && ch != '.' && ch != '/') {
            return false;
        }
    }
    return true;
}

bool valid_hex(std::string_view value, std::size_t size) {
    return value.size() == size && std::all_of(
        value.begin(), value.end(), [](const char ch) {
            return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
        });
}

bool valid_utf8_instruction(std::string_view value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto lead = static_cast<unsigned char>(value[i++]);
        if (lead < 0x80) continue;
        const int remaining = lead >= 0xC2 && lead <= 0xDF ? 1
            : lead >= 0xE0 && lead <= 0xEF ? 2 : lead >= 0xF0 && lead <= 0xF4 ? 3 : -1;
        if (remaining < 0 || value.size() - i < static_cast<std::size_t>(remaining)) return false;
        std::uint32_t scalar = lead & (remaining == 1 ? 0x1F : remaining == 2 ? 0x0F : 0x07);
        for (int n = 0; n < remaining; ++n) {
            const auto byte = static_cast<unsigned char>(value[i++]);
            if ((byte & 0xC0) != 0x80) return false;
            scalar = (scalar << 6) | (byte & 0x3F);
        }
        if ((remaining == 1 && scalar < 0x80) || (remaining == 2 && scalar < 0x800)
            || (remaining == 3 && scalar < 0x10000) || scalar > 0x10FFFF
            || (scalar >= 0xD800 && scalar <= 0xDFFF)) return false;
    }
    return true;
}

std::string base64_encode(std::string_view input) {
    std::string output;
    output.reserve(((input.size() + 2U) / 3U) * 4U);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const unsigned char byte : input) {
        accumulator = (accumulator << 8U) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output.push_back(kBase64Alphabet[(accumulator >> bits) & 0x3FU]);
        }
    }
    if (bits > 0) {
        output.push_back(kBase64Alphabet[(accumulator << (6 - bits)) & 0x3FU]);
    }
    while ((output.size() % 4U) != 0U) {
        output.push_back('=');
    }
    return output;
}

bool base64_decode(std::string_view input, std::string* output) {
    std::array<int, 256> reverse{};
    reverse.fill(-1);
    for (std::size_t i = 0; i < kBase64Alphabet.size(); ++i) {
        reverse[static_cast<unsigned char>(kBase64Alphabet[i])] = static_cast<int>(i);
    }
    output->clear();
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char ch : input) {
        if (ch == '=') {
            break;
        }
        const int value = reverse[static_cast<unsigned char>(ch)];
        if (value < 0) {
            return false;
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output->push_back(static_cast<char>((accumulator >> bits) & 0xFFU));
        }
    }
    // Reject noncanonical padding, trailing garbage and discarded low bits.
    return base64_encode(*output) == input;
}

}  // namespace

std::string_view to_string(AgentMessageTypeV1 type) {
    switch (type) {
        case AgentMessageTypeV1::kCapabilities: return "agent_capabilities";
        case AgentMessageTypeV1::kProjectCatalog: return "agent_project_catalog";
        case AgentMessageTypeV1::kTaskCreate: return "agent_task_create";
        case AgentMessageTypeV1::kTurnStart: return "agent_turn_start";
        case AgentMessageTypeV1::kTurnSteer: return "agent_turn_steer";
        case AgentMessageTypeV1::kTurnInterrupt: return "agent_turn_interrupt";
        case AgentMessageTypeV1::kTaskSyncRequest: return "agent_task_sync_request";
        case AgentMessageTypeV1::kTaskSnapshot: return "agent_task_snapshot";
        case AgentMessageTypeV1::kEvent: return "agent_event";
        case AgentMessageTypeV1::kEventAck: return "agent_event_ack";
        case AgentMessageTypeV1::kApprovalRequest: return "agent_approval_request";
        case AgentMessageTypeV1::kApprovalDecision: return "agent_approval_decision";
        case AgentMessageTypeV1::kTaskComplete: return "agent_task_complete";
        case AgentMessageTypeV1::kTaskError: return "agent_task_error";
    }
    return "agent_task_error";
}

std::string_view to_string(AgentProviderKindV1 provider) {
    switch (provider) {
        case AgentProviderKindV1::kNone: return "none";
        case AgentProviderKindV1::kCodex: return "codex";
        case AgentProviderKindV1::kCursor: return "cursor";
    }
    return "none";
}

std::string_view to_string(AgentProviderReadinessV1 readiness) {
    switch (readiness) {
        case AgentProviderReadinessV1::kCliMissing: return "cli_missing";
        case AgentProviderReadinessV1::kNotAuthenticated: return "not_authenticated";
        case AgentProviderReadinessV1::kReady: return "ready";
        case AgentProviderReadinessV1::kProbeFailed: return "probe_failed";
        case AgentProviderReadinessV1::kModelUnavailable: return "model_unavailable";
    }
    return "probe_failed";
}

std::string_view to_string(AgentWorkDirectoryModeV1 mode) {
    switch (mode) {
        case AgentWorkDirectoryModeV1::kIsolatedWorktree: return "isolated_worktree";
        case AgentWorkDirectoryModeV1::kDirectWorkspace: return "direct_workspace";
    }
    return "isolated_worktree";
}

std::string_view to_string(AgentTaskStateV1 state) {
    switch (state) {
        case AgentTaskStateV1::kUnavailable: return "unavailable";
        case AgentTaskStateV1::kIdle: return "idle";
        case AgentTaskStateV1::kQueued: return "queued";
        case AgentTaskStateV1::kStarting: return "starting";
        case AgentTaskStateV1::kRunning: return "running";
        case AgentTaskStateV1::kAwaitingApproval: return "awaiting_approval";
        case AgentTaskStateV1::kPaused: return "paused";
        case AgentTaskStateV1::kCompleted: return "completed";
        case AgentTaskStateV1::kFailed: return "failed";
        case AgentTaskStateV1::kInterrupted: return "interrupted";
    }
    return "unavailable";
}

std::string_view to_string(AgentApprovalDecisionV1 decision) {
    switch (decision) {
        case AgentApprovalDecisionV1::kNone: return "none";
        case AgentApprovalDecisionV1::kAccept: return "accept";
        case AgentApprovalDecisionV1::kReject: return "reject";
    }
    return "none";
}

bool validate_agent_message_v1(const AgentMessageEnvelopeV1& message, std::string* error) {
    if (message.schema_version != kSchemaVersionV1) {
        assign_error("unsupported agent schema version", error);
        return false;
    }
    if (!valid_token(message.session_epoch, 128) || message.message_id == 0) {
        assign_error("invalid agent envelope identity", error);
        return false;
    }
    if (!valid_token(message.task_id, 128, true)
        || !valid_token(message.request_id, 128, true)
        || !valid_token(message.supersedes_request_id, 128, true)
        || !valid_token(message.project_id, 128, true)
        || !valid_token(message.model, 128, true)
        || !valid_token(message.event_kind, 128, true)
        || !valid_token(message.error_code, 128, true)) {
        assign_error("invalid agent token field", error);
        return false;
    }
    const bool has_evidence_name = !message.evidence_manifest_name.empty();
    const bool has_evidence_hash = !message.evidence_sha256.empty();
    if (has_evidence_name != has_evidence_hash
        || (has_evidence_name
            && (!valid_token(message.evidence_manifest_name, 256)
                || message.evidence_manifest_name.find('/') != std::string::npos
                || message.evidence_manifest_name.find('\\') != std::string::npos
                || !valid_hex(message.evidence_sha256, 64)))) {
        assign_error("invalid Agent evidence reference", error);
        return false;
    }
    const bool instruction = message.type == AgentMessageTypeV1::kTaskCreate
        || message.type == AgentMessageTypeV1::kTurnStart
        || message.type == AgentMessageTypeV1::kTurnSteer;
    // text is Protobuf bytes because output chunks can split a code point, but
    // task instructions are complete UTF-8 documents, not opaque byte chunks.
    if (instruction && !valid_utf8_instruction(message.text)) {
        assign_error("agent instruction is not valid UTF-8", error);
        return false;
    }
    const std::size_t text_limit = instruction
        ? kMaxAgentInstructionBytes : kMaxAgentEventChunkBytes;
    if (message.text.size() > text_limit || message.display_name.size() > 256U) {
        assign_error("agent text field exceeds limit", error);
        return false;
    }
    if ((message.type == AgentMessageTypeV1::kTaskCreate
         || message.type == AgentMessageTypeV1::kTurnStart)
        && (message.task_id.empty() || message.project_id.empty()
            || message.provider == AgentProviderKindV1::kNone || message.text.empty())) {
        assign_error("agent task request is incomplete", error);
        return false;
    }
    if (message.type == AgentMessageTypeV1::kApprovalDecision
        && (message.request_id.empty()
            || message.approval_decision == AgentApprovalDecisionV1::kNone)) {
        assign_error("agent approval decision is incomplete", error);
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

std::optional<bool> agent_capabilities_authorization_update_v1(
    const AgentMessageEnvelopeV1& message) noexcept {
    if (message.type != AgentMessageTypeV1::kCapabilities || !message.complete) {
        return std::nullopt;
    }
    return message.available;
}

std::string serialize_local_runtime_agent_frame_v1(const AgentMessageEnvelopeV1& message) {
    const auto wire = serialize_agent_message_v1(message);
    if (wire.empty()) return {};
    auto frame = std::string(kLocalFramePrefix) + base64_encode(wire);
    if (frame.size() > kMaxLocalRuntimeAgentFrameBytes) return {};
    return frame;
}

ParseResult<AgentMessageEnvelopeV1> parse_local_runtime_agent_frame_v1(std::string_view line) {
    ParseResult<AgentMessageEnvelopeV1> result;
    if (!line.starts_with(kLocalFramePrefix) || line.size() > kMaxLocalRuntimeAgentFrameBytes) {
        result.error = "invalid local runtime agent frame";
        return result;
    }
    std::string decoded;
    if (!base64_decode(line.substr(kLocalFramePrefix.size()), &decoded)) {
        result.error = "invalid local runtime agent frame base64";
        return result;
    }
    return parse_agent_message_v1(decoded);
}

bool AgentEpochGuardV1::accept(const AgentMessageEnvelopeV1& message, std::string* error) {
    if (!validate_agent_message_v1(message, error)) {
        return false;
    }
    if (epoch_.empty()) {
        epoch_ = message.session_epoch;
    } else if (epoch_ != message.session_epoch) {
        assign_error("agent message epoch mismatch", error);
        return false;
    }
    if (message.message_id <= last_message_id_) {
        assign_error("agent message replay or reordering detected", error);
        return false;
    }
    last_message_id_ = message.message_id;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void AgentEpochGuardV1::reset() noexcept {
    epoch_.clear();
    last_message_id_ = 0;
}

const std::string& AgentEpochGuardV1::epoch() const noexcept {
    return epoch_;
}

std::uint64_t AgentEpochGuardV1::last_message_id() const noexcept {
    return last_message_id_;
}

}  // namespace redclaw::protocol
