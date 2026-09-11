#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace redclaw::service {

enum class LocalHelperRole {
    kOff,
    kClient,
    kLanHelper,
    kRelay,
};

enum class LocalHelperPathStatus {
    kUnavailable,
    kMissing,
    kNotDirectory,
    kReadOnly,
    kWritable,
};

struct LocalHelperPathProbe {
    LocalHelperPathStatus status = LocalHelperPathStatus::kUnavailable;
    std::filesystem::path path;
    std::string detail;
    bool exists = false;
    bool writable = false;
};

// Session codes stay usable across sessions, so a peer that already exited can
// leave a record behind that still decrypts. Both sides derive the sealing
// time from the published expiry, which only works while every publisher uses
// the same TTL.
inline constexpr std::uint64_t kDhtRecordTtlSeconds = 300;
inline constexpr std::size_t kDirectDhtEncryptedBlobBudgetBytes = 700;

struct DhtSignalSnapshot {
    std::string role;
    std::string description_type;
    std::string description_sdp;
    // Random identifier owned by one runtime process. It is stable across ICE
    // generations in that process and changes after a process restart. This is
    // correlation metadata only; it is not an authentication credential.
    std::string publisher_instance_id;
    // Host-owned ICE generation. Controller echoes the generation carried by
    // the offer it answers; zero is never a valid v4 generation.
    std::uint64_t generation = 0;
    // Correlates a Controller connect action with the Host offer it caused.
    // Required for request/offer/answer records in v4.
    std::string connection_request_tag;
    // Identifies the remote description this snapshot answers, so an answer left
    // behind by an earlier session can never be mistaken for a reply to the
    // offer that is currently being advertised. Empty on offers.
    std::string answered_description_tag;
    // Host sets this only after applying the matching Controller answer.
    // Controller observes it but never echoes it or gates candidates on it.
    std::string acknowledged_answer_tag;
    bool candidates_complete = false;
    std::vector<std::string> candidate_lines;
    std::uint64_t revision = 0;
    std::uint64_t expires_at_unix = 0;
};

struct DhtInitialSnapshotPlan {
    std::vector<std::string> candidate_lines;
    std::size_t full_encrypted_blob_bytes = 0;
    bool full_snapshot_fits_direct = false;
};

// Stable content identity for pending publication retries, excluding record
// revision/expiry. Only genuine local snapshot changes require a new revision.
[[nodiscard]] std::string make_dht_signal_publication_key(const DhtSignalSnapshot& snapshot);

// Derives the tag that identifies a description within a rendezvous exchange.
[[nodiscard]] std::string derive_dht_description_tag(std::string_view description_sdp);

// V4 publisher identifiers are 128 random bits encoded as 32 lowercase hex
// characters. Generation failure is fatal for DHT runtime startup because a
// missing identity would make Host-restart recovery ambiguous.
[[nodiscard]] bool is_valid_dht_publisher_instance_id(std::string_view instance_id) noexcept;
[[nodiscard]] std::optional<std::string> make_dht_publisher_instance_id(
    std::string* error_detail = nullptr);

// Seeds the per-process logical revision from wall-clock time. A peer may retain
// the last accepted revision while the publisher process restarts, so restarting
// at revision one would make every fresh record look stale until the counter
// catches up.
[[nodiscard]] std::uint64_t make_dht_publish_revision_floor(std::uint64_t unix_time_ms);

// Answers must retain their offer correlation on every publication, including
// keep-alive refreshes that do not rebuild the local SDP.
[[nodiscard]] std::string select_dht_answered_description_tag(
    std::string_view description_type,
    std::string_view offer_description_tag);

struct DhtRendezvousConfig {
    std::string session_code;
    std::string pairing_secret;
    std::uint64_t now_unix = 0;
    std::uint64_t ttl_seconds = kDhtRecordTtlSeconds;
};

struct DhtEncryptedRecord {
    std::string topic_hex;
    std::string lane;
    std::string encrypted_blob;
    std::uint64_t revision = 0;
    std::uint64_t expires_at_unix = 0;
};

enum class DhtPublishState { kPending, kSucceeded, kRetryableFailure, kPermanentFailure };
enum class DhtPublishFailure { kNone, kExpired };
struct DhtPublishResult {
    DhtPublishState state = DhtPublishState::kPermanentFailure;
    DhtPublishFailure failure = DhtPublishFailure::kNone;
    std::uint64_t expires_at_unix = 0;
    DhtPublishResult() = default;
    DhtPublishResult(DhtPublishState value, DhtPublishFailure reason = DhtPublishFailure::kNone)
        : state(value), failure(reason) {}
    // Synchronous local stores use the same typed contract; false is terminal
    // unless the asynchronous adapter explicitly reports pending/retryable.
    DhtPublishResult(bool succeeded) : state(succeeded ? DhtPublishState::kSucceeded : DhtPublishState::kPermanentFailure) {}
    operator bool() const { return state == DhtPublishState::kSucceeded; }
};

class IDhtRendezvousStore {
public:
    virtual ~IDhtRendezvousStore() = default;
    virtual void advance() {}

    [[nodiscard]] virtual DhtPublishResult publish(
        const DhtEncryptedRecord& record,
        std::string_view routing_token,
        std::string* error_detail = nullptr) = 0;

    [[nodiscard]] virtual std::optional<DhtEncryptedRecord> fetch(
        std::string_view topic_hex,
        std::string_view lane,
        std::uint64_t after_revision,
        std::string_view routing_token,
        std::string* error_detail = nullptr) = 0;
};

class InMemoryDhtRendezvousStore final : public IDhtRendezvousStore {
public:
    [[nodiscard]] DhtPublishResult publish(
        const DhtEncryptedRecord& record,
        std::string_view routing_token,
        std::string* error_detail = nullptr) override;

    [[nodiscard]] std::optional<DhtEncryptedRecord> fetch(
        std::string_view topic_hex,
        std::string_view lane,
        std::uint64_t after_revision,
        std::string_view routing_token,
        std::string* error_detail = nullptr) override;

private:
    std::unordered_map<std::string, DhtEncryptedRecord> records_;
};

class FileBackedDhtRendezvousStore final : public IDhtRendezvousStore {
public:
    explicit FileBackedDhtRendezvousStore(std::filesystem::path root_directory);

    [[nodiscard]] DhtPublishResult publish(
        const DhtEncryptedRecord& record,
        std::string_view routing_token,
        std::string* error_detail = nullptr) override;

    [[nodiscard]] std::optional<DhtEncryptedRecord> fetch(
        std::string_view topic_hex,
        std::string_view lane,
        std::uint64_t after_revision,
        std::string_view routing_token,
        std::string* error_detail = nullptr) override;

    [[nodiscard]] std::optional<DhtEncryptedRecord> fetch_latest_indirect_blob(
        std::string_view topic_hex,
        std::string_view lane,
        std::uint64_t after_revision,
        std::string* error_detail = nullptr);

private:
    [[nodiscard]] std::filesystem::path record_path(std::string_view topic_hex, std::string_view lane) const;

    std::filesystem::path root_directory_;
};

class IndirectDhtRendezvousStore final : public IDhtRendezvousStore {
public:
    IndirectDhtRendezvousStore(
        std::unique_ptr<IDhtRendezvousStore> primary_store,
        std::unique_ptr<IDhtRendezvousStore> secondary_store);
    void advance() override;

    [[nodiscard]] DhtPublishResult publish(
        const DhtEncryptedRecord& record,
        std::string_view routing_token,
        std::string* error_detail = nullptr) override;

    [[nodiscard]] std::optional<DhtEncryptedRecord> fetch(
        std::string_view topic_hex,
        std::string_view lane,
        std::uint64_t after_revision,
        std::string_view routing_token,
        std::string* error_detail = nullptr) override;

private:
    std::unique_ptr<IDhtRendezvousStore> primary_store_;
    std::unique_ptr<IDhtRendezvousStore> secondary_store_;
    std::string cached_chunk_publish_key_;
    std::vector<bool> cached_chunk_publish_success_;
    std::size_t cached_chunk_cursor_ = 0;
};

struct LibtorrentDhtRendezvousStoreOptions {
    std::vector<std::string> bootstrap_nodes;
    std::string listen_address;
    std::string listen_ipv6_address;
    int listen_port = 0;
    // Lifetime of one asynchronous native lookup+put, not a blocking wait or
    // polling interval. Public DHT traversal commonly completes after 8 seconds.
    int publish_timeout_milliseconds = 120000;
    int fetch_timeout_milliseconds = 0;
    bool enable_ipv6 = true;
};

struct LibtorrentDhtDiagnosticsSnapshot {
    std::size_t publish_records = 0, publish_in_flight = 0, publish_records_peak = 0;
    std::uint64_t publish_late_events = 0, publish_backpressure = 0;
    int listen_port = 0;
    bool listen_port_auto_selected = false;
    bool listen_ready = false, listen_startup_failed = false;
    unsigned listen_port_probe_attempts = 0;
    std::string listen_port_selection_error;
    int listen_succeeded_count = 0;
    int listen_failed_count = 0;
    int bootstrap_count = 0;
    int dht_error_count = 0;
    int external_ip_count = 0;
    int dht_log_count = 0;
    int put_alert_count = 0;
    int last_put_num_success = -1;
    std::uint64_t mutable_get_started_count = 0;
    std::uint64_t mutable_get_response_count = 0;
    std::uint64_t mutable_get_authoritative_count = 0;
    std::uint64_t mutable_get_non_authoritative_count = 0;
    std::uint64_t mutable_get_pending_poll_count = 0;
    std::uint64_t mutable_get_cache_hit_count = 0;
    int mutable_get_pending_count = 0;
    std::int64_t last_mutable_get_elapsed_ms = -1;
    std::int64_t max_mutable_get_elapsed_ms = -1;
    std::int64_t last_mutable_get_sequence = -1;
    int dht_nodes = -1;
    int dht_node_cache = -1;
    bool bootstrap_seen = false;
    std::string listen_address;
    std::string listen_interfaces;
    std::string last_listen_success;
    std::string last_listen_failure;
    std::string last_dht_error;
    std::string last_external_ip;
    std::vector<std::string> recent_dht_logs;
    std::vector<std::string> bootstrap_nodes;
};

class LibtorrentDhtRendezvousStore final : public IDhtRendezvousStore {
public:
    explicit LibtorrentDhtRendezvousStore(LibtorrentDhtRendezvousStoreOptions options = {});
    ~LibtorrentDhtRendezvousStore() override;
    void advance() override;

    [[nodiscard]] DhtPublishResult publish(
        const DhtEncryptedRecord& record,
        std::string_view routing_token,
        std::string* error_detail = nullptr) override;

    [[nodiscard]] std::optional<DhtEncryptedRecord> fetch(
        std::string_view topic_hex,
        std::string_view lane,
        std::uint64_t after_revision,
        std::string_view routing_token,
        std::string* error_detail = nullptr) override;

    [[nodiscard]] int listen_port() const;
    void pump_alerts(std::chrono::milliseconds max_wait);
    [[nodiscard]] LibtorrentDhtDiagnosticsSnapshot diagnostics_snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class DhtRendezvousClient final {
public:
    explicit DhtRendezvousClient(IDhtRendezvousStore& store);

    [[nodiscard]] DhtPublishResult publish_signal_snapshot(
        const DhtRendezvousConfig& config,
        const DhtSignalSnapshot& snapshot,
        std::string* error_detail = nullptr);

    [[nodiscard]] std::optional<DhtSignalSnapshot> fetch_signal_snapshot(
        const DhtRendezvousConfig& config,
        std::string_view lane,
        std::uint64_t after_revision,
        std::string* error_detail = nullptr);

private:
    IDhtRendezvousStore& store_;
    std::string cached_publish_key_;
    std::optional<DhtEncryptedRecord> cached_publish_record_;
};

[[nodiscard]] bool parse_local_helper_role(
    std::string_view value,
    LocalHelperRole* role,
    std::string* error_detail = nullptr);

[[nodiscard]] std::string local_helper_role_to_string(LocalHelperRole role);
[[nodiscard]] std::string local_helper_path_status_to_string(LocalHelperPathStatus status);

[[nodiscard]] std::string derive_dht_rendezvous_topic_hex(
    std::string_view session_code,
    std::string_view pairing_secret);

[[nodiscard]] std::string derive_dht_rendezvous_routing_token_hex(
    std::string_view session_code,
    std::string_view pairing_secret,
    std::string_view lane);

[[nodiscard]] LocalHelperPathProbe probe_local_helper_path(const std::filesystem::path& path);

[[nodiscard]] bool candidate_looks_ipv6(std::string_view candidate_sdp);
[[nodiscard]] bool candidate_looks_server_reflexive(std::string_view candidate_sdp);
[[nodiscard]] bool candidate_looks_relay(std::string_view candidate_sdp);
[[nodiscard]] std::vector<std::string> prioritize_dht_candidate_lines(
    const std::vector<std::string>& candidate_lines);
[[nodiscard]] std::vector<std::string> merge_dht_candidate_snapshots(
    const std::vector<std::string>& published_candidate_lines,
    const std::vector<std::string>& gathered_candidate_lines);
[[nodiscard]] std::optional<std::size_t> estimate_dht_encrypted_blob_bytes(
    const DhtSignalSnapshot& snapshot,
    std::string* error_detail = nullptr);
[[nodiscard]] bool dht_signal_snapshot_fits_direct_record(
    const DhtSignalSnapshot& snapshot,
    std::size_t* encrypted_blob_bytes = nullptr,
    std::string* error_detail = nullptr);
[[nodiscard]] std::optional<DhtInitialSnapshotPlan> plan_dht_initial_snapshot(
    const DhtSignalSnapshot& description_snapshot,
    const std::vector<std::string>& candidate_lines,
    std::string* error_detail = nullptr);

[[nodiscard]] std::vector<std::string> default_dht_bootstrap_nodes();
[[nodiscard]] std::vector<std::string> cached_dht_bootstrap_fallback_nodes(std::string_view node);
[[nodiscard]] std::vector<std::string> normalize_dht_bootstrap_nodes(
    const std::vector<std::string>& requested);

}  // namespace redclaw::service
