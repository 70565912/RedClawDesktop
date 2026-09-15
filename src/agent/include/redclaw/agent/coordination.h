#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "redclaw/protocol/agent_protocol.h"

namespace redclaw::agent {

enum class CoordinationAuthorityV1 {
    kNone,
    kNormalAgent,
    kDebugBridge,
    kOutOfBand,
};

enum class CoordinationRecordTypeV1 {
    kAuthorityTransfer,
    kRequest,
    kReply,
    kSupersede,
    kAuthorization,
    kTerminal,
    kEvidence,
    kLifecycle,
};

enum class CoordinationRequestStateV1 {
    kAccepted,
    kSyncing,
    kActive,
    kPaused,
    kSuperseded,
    kCompleted,
    kFailed,
    kInterrupted,
};

struct CoordinationJournalRecordV1 {
    std::uint64_t sequence = 0;
    std::uint64_t recorded_at_ms = 0;
    CoordinationRecordTypeV1 record_type = CoordinationRecordTypeV1::kRequest;
    CoordinationAuthorityV1 authority = CoordinationAuthorityV1::kNone;
    CoordinationRequestStateV1 request_state = CoordinationRequestStateV1::kAccepted;
    std::string request_id;
    std::string reply_to_request_id;
    std::string supersedes_request_id;
    std::string task_id;
    std::string request_digest_sha256;
    std::string authorization_state;
    std::string git_sha;
    std::string executable_sha256;
    std::string terminal_result;
    std::string evidence_manifest_name;
    std::string evidence_sha256;
    std::uint64_t acknowledged_event_sequence = 0;
};

class CoordinationJournalV1 final {
public:
    explicit CoordinationJournalV1(std::filesystem::path path);

    [[nodiscard]] bool reload(std::string* error = nullptr);
    [[nodiscard]] bool append(
        CoordinationJournalRecordV1 record,
        std::string* error = nullptr);
    [[nodiscard]] bool append_if_authority(
        CoordinationJournalRecordV1 record,
        CoordinationAuthorityV1 expected_authority,
        std::string* error = nullptr);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] const std::vector<CoordinationJournalRecordV1>& records() const noexcept;
    [[nodiscard]] std::uint64_t next_sequence() const noexcept;
    [[nodiscard]] std::uint64_t parsed_record_count() const noexcept { return parsed_record_count_; }
    [[nodiscard]] bool invalid() const noexcept { return invalid_; }

private:
    [[nodiscard]] bool reload_unlocked(std::string* error);
    [[nodiscard]] bool append_unlocked(
        CoordinationJournalRecordV1 record,
        std::string* error);
    void index_record(const CoordinationJournalRecordV1& record);

    std::filesystem::path path_;
    std::vector<CoordinationJournalRecordV1> records_;
    std::uint64_t next_sequence_ = 1;
    std::uint64_t read_offset_ = 0;
    std::uint64_t parsed_record_count_ = 0;
    std::string file_identity_;
    CoordinationAuthorityV1 current_authority_ = CoordinationAuthorityV1::kNone;
    bool invalid_ = false;
    std::unordered_map<std::string, bool> pending_approvals_;
    std::unordered_set<std::string> decided_approvals_;
};

struct NormalAgentControlConfigV1 {
    std::filesystem::path journal_path;
    std::string git_sha;
    std::string executable_sha256;
};

class NormalAgentControlStateV1 final {
public:
    explicit NormalAgentControlStateV1(NormalAgentControlConfigV1 config);

    [[nodiscard]] bool initialize(std::string* error = nullptr);
    [[nodiscard]] bool refresh(std::string* error = nullptr);
    [[nodiscard]] bool transfer_authority(
        CoordinationAuthorityV1 expected,
        CoordinationAuthorityV1 next,
        bool explicitly_selected,
        bool normal_agent_available,
        bool debug_bridge_available,
        std::string_view reason,
        std::uint64_t now_ms,
        std::string* error = nullptr);

    [[nodiscard]] bool prepare_outbound(
        const redclaw::protocol::AgentMessageEnvelopeV1& message,
        std::string_view supersedes_request_id,
        std::string_view authorization_state,
        std::uint64_t now_ms,
        std::string* error = nullptr);
    [[nodiscard]] bool observe_remote(
        const redclaw::protocol::AgentMessageEnvelopeV1& message,
        std::uint64_t now_ms,
        std::string* error = nullptr,
        bool* apply_to_view = nullptr);
    [[nodiscard]] std::vector<redclaw::protocol::AgentMessageEnvelopeV1>
    take_sync_requests(
        std::string_view local_epoch,
        std::uint64_t* next_message_id,
        std::uint64_t now_ms);

    [[nodiscard]] std::optional<redclaw::protocol::AgentMessageEnvelopeV1>
    task_snapshot(std::string_view task_id) const;
    [[nodiscard]] CoordinationAuthorityV1 authority() const noexcept;
    [[nodiscard]] bool sync_required() const noexcept;
    void request_sync(std::string_view task_id = {});
    [[nodiscard]] std::uint64_t acknowledged_event_sequence(
        std::string_view task_id) const noexcept;
    [[nodiscard]] const CoordinationJournalV1& journal() const noexcept;

private:
    struct ProviderCapability {
        bool available = false;
        redclaw::protocol::AgentProviderReadinessV1 readiness =
            redclaw::protocol::AgentProviderReadinessV1::kProbeFailed;
        std::unordered_set<std::string> models;
    };
    struct ProjectCapability {
        bool git_repository = false;
    };
    struct RequestState {
        std::string task_id;
        CoordinationRequestStateV1 state = CoordinationRequestStateV1::kAccepted;
        bool awaiting_approval = false;
        bool approval_decided = false;
    };

    [[nodiscard]] bool rebuild(std::string* error);
    [[nodiscard]] bool append_record(
        CoordinationJournalRecordV1 record,
        std::string* error);
    [[nodiscard]] bool validate_catalog_request(
        const redclaw::protocol::AgentMessageEnvelopeV1& message,
        std::string* error) const;

    NormalAgentControlConfigV1 config_;
    CoordinationJournalV1 journal_;
    CoordinationAuthorityV1 authority_ = CoordinationAuthorityV1::kNone;
    std::unordered_map<std::string, RequestState> requests_;
    std::unordered_map<std::string, redclaw::protocol::AgentMessageEnvelopeV1> tasks_;
    std::unordered_map<std::string, std::uint64_t> acknowledged_sequences_;
    std::unordered_map<int, ProviderCapability> providers_;
    std::unordered_map<std::string, ProjectCapability> projects_;
    std::unordered_set<std::string> pending_sync_tasks_;
    redclaw::protocol::AgentEpochGuardV1 incoming_guard_;
    std::string remote_epoch_;
    std::optional<bool> remote_authorized_;
    bool sync_required_ = false;
    bool sync_requests_issued_ = false;
    std::uint64_t last_sync_request_ms_ = 0;
    std::size_t applied_record_count_ = 0;
};

enum class AgentLifecycleActionV1 {
    kPublish,
    kStop,
    kRestart,
};

struct AgentLifecycleRequestV1 {
    std::string request_id;
    AgentLifecycleActionV1 action = AgentLifecycleActionV1::kPublish;
    std::string expected_git_sha;
    std::string candidate_executable_sha256;
    std::string target_path_sha256;
    std::uint64_t target_pid = 0;
    bool build_gate_passed = false;
    bool focused_test_gate_passed = false;
};

struct AgentLifecycleObservedStateV1 {
    std::uint64_t now_ms = 0;
    std::string git_sha;
    std::string candidate_executable_sha256;
    std::string target_path_sha256;
    std::uint64_t target_pid = 0;
};

struct AgentLifecycleApprovalV1 {
    std::string approval_id;
    std::string request_digest_sha256;
    std::uint64_t expires_at_ms = 0;
    bool consumed = false;
};

class AgentLifecycleApprovalGateV1 final {
public:
    [[nodiscard]] bool authorize(
        const AgentLifecycleRequestV1& request,
        std::string approval_id,
        std::uint64_t now_ms,
        std::uint64_t ttl_ms,
        bool operator_approved,
        AgentLifecycleApprovalV1* approval,
        std::string* error = nullptr);
    [[nodiscard]] bool consume(
        const AgentLifecycleRequestV1& request,
        const AgentLifecycleObservedStateV1& observed,
        std::string_view approval_id,
        std::string* error = nullptr);

private:
    std::unordered_map<std::string, AgentLifecycleApprovalV1> approvals_;
};

[[nodiscard]] std::string sha256_hex(std::string_view value);
[[nodiscard]] std::string lifecycle_request_digest_v1(
    const AgentLifecycleRequestV1& request);
[[nodiscard]] std::string_view to_string(CoordinationAuthorityV1 value);
[[nodiscard]] std::string_view to_string(CoordinationRecordTypeV1 value);
[[nodiscard]] std::string_view to_string(CoordinationRequestStateV1 value);
[[nodiscard]] std::string_view to_string(AgentLifecycleActionV1 value);

}  // namespace redclaw::agent
