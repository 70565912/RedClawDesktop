#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "redclaw/protocol/protocol_module.h"

namespace redclaw::protocol {

inline constexpr std::size_t kMaxAgentMessageBytes = 64U * 1024U;
inline constexpr std::size_t kMaxAgentInstructionBytes = 16U * 1024U;
inline constexpr std::size_t kMaxAgentEventChunkBytes = 8U * 1024U;
inline constexpr std::size_t kMaxLocalRuntimeAgentFrameBytes = 64U * 1024U;

enum class AgentMessageTypeV1 {
    kCapabilities,
    kProjectCatalog,
    kTaskCreate,
    kTurnStart,
    kTurnSteer,
    kTurnInterrupt,
    kTaskSyncRequest,
    kTaskSnapshot,
    kEvent,
    kEventAck,
    kApprovalRequest,
    kApprovalDecision,
    kTaskComplete,
    kTaskError,
};

enum class AgentProviderKindV1 {
    kNone,
    kCodex,
    kCursor,
};

enum class AgentProviderReadinessV1 {
    kCliMissing,
    kNotAuthenticated,
    kReady,
    kProbeFailed,
    kModelUnavailable,
};

enum class AgentWorkDirectoryModeV1 {
    kIsolatedWorktree,
    kDirectWorkspace,
};

enum class AgentTaskStateV1 {
    kUnavailable,
    kIdle,
    kQueued,
    kStarting,
    kRunning,
    kAwaitingApproval,
    kPaused,
    kCompleted,
    kFailed,
    kInterrupted,
};

enum class AgentApprovalDecisionV1 {
    kNone,
    kAccept,
    kReject,
};

struct AgentMessageEnvelopeV1 {
    int schema_version = kSchemaVersionV1;
    std::string session_epoch;
    std::uint64_t message_id = 0;
    std::uint64_t sent_at_ms = 0;
    std::string task_id;
    std::string request_id;
    std::string supersedes_request_id;
    std::uint64_t event_sequence = 0;
    AgentMessageTypeV1 type = AgentMessageTypeV1::kCapabilities;

    AgentProviderKindV1 provider = AgentProviderKindV1::kNone;
    AgentProviderReadinessV1 provider_readiness =
        AgentProviderReadinessV1::kProbeFailed;
    AgentWorkDirectoryModeV1 work_directory_mode =
        AgentWorkDirectoryModeV1::kIsolatedWorktree;
    AgentTaskStateV1 task_state = AgentTaskStateV1::kUnavailable;
    AgentApprovalDecisionV1 approval_decision = AgentApprovalDecisionV1::kNone;

    std::string model;
    std::string project_id;
    std::string display_name;
    std::string event_kind;
    std::string error_code;
    std::string evidence_manifest_name;
    std::string evidence_sha256;
    std::string text;
    std::uint64_t acknowledged_event_sequence = 0;
    bool available = false;
    bool complete = false;
    bool gap = false;
    bool git_repository = false;
    bool requires_turn_approval = false;
    bool supports_structured_approval = false;
};

class AgentEpochGuardV1 final {
public:
    [[nodiscard]] bool accept(
        const AgentMessageEnvelopeV1& message,
        std::string* error = nullptr);
    void reset() noexcept;

    [[nodiscard]] const std::string& epoch() const noexcept;
    [[nodiscard]] std::uint64_t last_message_id() const noexcept;

private:
    std::string epoch_;
    std::uint64_t last_message_id_ = 0;
};

[[nodiscard]] bool validate_agent_message_v1(
    const AgentMessageEnvelopeV1& message,
    std::string* error = nullptr);
[[nodiscard]] std::string serialize_agent_message_v1(
    const AgentMessageEnvelopeV1& message);
// Expanded size for queue accounting, independent of compression ratio.
[[nodiscard]] std::size_t agent_message_protobuf_size_v1(
    const AgentMessageEnvelopeV1& message);
[[nodiscard]] ParseResult<AgentMessageEnvelopeV1> parse_agent_message_v1(
    std::string_view serialized);
[[nodiscard]] std::optional<bool> agent_capabilities_authorization_update_v1(
    const AgentMessageEnvelopeV1& message) noexcept;
[[nodiscard]] std::string serialize_local_runtime_agent_frame_v1(
    const AgentMessageEnvelopeV1& message);
[[nodiscard]] ParseResult<AgentMessageEnvelopeV1> parse_local_runtime_agent_frame_v1(
    std::string_view line);

[[nodiscard]] std::string_view to_string(AgentMessageTypeV1 type);
[[nodiscard]] std::string_view to_string(AgentProviderKindV1 provider);
[[nodiscard]] std::string_view to_string(AgentProviderReadinessV1 readiness);
[[nodiscard]] std::string_view to_string(AgentWorkDirectoryModeV1 mode);
[[nodiscard]] std::string_view to_string(AgentTaskStateV1 state);
[[nodiscard]] std::string_view to_string(AgentApprovalDecisionV1 decision);

}  // namespace redclaw::protocol
