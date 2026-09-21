#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace redclaw::service {

enum class ConnectionNegotiationRole {
    kHost,
    kController,
};

enum class ConnectionNegotiationPhase {
    kIdle,
    kRequestReady,
    kRequestPublished,
    kRequestReceived,
    kOfferReady,
    kOfferApplied,
    kAnswerReady,
    kAnswerPublished,
    kAnswerApplied,
    kIceConnecting,
    kConnected,
    kFailed,
};

enum class ConnectionNegotiationObservation {
    kAccepted,
    kDuplicate,
    kRejectedRole,
    kRejectedInvalid,
    kRejectedStaleGeneration,
    kRejectedGenerationConflict,
    kRejectedOfferMismatch,
    kRejectedAnswerMismatch,
    kRejectedTerminalState,
};

struct ConnectionNegotiationResult {
    ConnectionNegotiationObservation observation = ConnectionNegotiationObservation::kRejectedInvalid;
    bool generation_changed = false;
    bool state_changed = false;

    [[nodiscard]] bool accepted() const noexcept {
        return observation == ConnectionNegotiationObservation::kAccepted
            || observation == ConnectionNegotiationObservation::kDuplicate;
    }
};

enum class DhtIceFailureRecoveryAction {
    kNone,
    kWaitBackoff,
    kRebuildControllerRequest,
    kWaitForHostRepair,
};

struct DhtIceFailureRecoveryInput {
    ConnectionNegotiationRole role = ConnectionNegotiationRole::kHost;
    bool transport_failed = false;
    bool connected_once = false;
    bool automatic_recovery_owned = false;
    bool remote_description_applied = false;
    // Monotonic elapsed time since first observing failure in this attempt.
    // Late signaling must never restart this timer.
    std::uint64_t failure_elapsed_ms = 0;
    std::uint32_t repair_attempts = 0;
};

enum class AutomaticReconnectAction { kNone, kScheduled, kAttemptFailed, kRebuild };

// Serialized by the runtime callback mutex; the active flag owns both flight
// and backoff. Progress can renew each phase once, within the total deadline.
struct AutomaticReconnectSchedule {
    bool active = false;
    bool in_flight = false;
    std::uint32_t attempt_count = 0;
    std::uint64_t retry_after_ms = 0;
    std::uint64_t deadline_ms = 0;
    std::uint64_t total_deadline_ms = 0;
    ConnectionNegotiationPhase phase = ConnectionNegotiationPhase::kIdle;
    std::uint64_t generation = 0;

    [[nodiscard]] AutomaticReconnectAction tick(
        std::uint64_t now_ms, std::uint64_t stage_window_ms, bool transport_failed);
    bool observe_progress(ConnectionNegotiationPhase next_phase, std::uint64_t next_generation,
        std::uint64_t now_ms, std::uint64_t stage_window_ms);
};

struct DhtIceFailureRecoveryDecision {
    DhtIceFailureRecoveryAction action = DhtIceFailureRecoveryAction::kNone;
    bool may_apply_remote_candidates = true;
};

enum class PersistentHostOfferAdoption {
    kRejected,
    kFreshController,
    kRestartedHost,
    kNewerUnauthenticatedHost,
};

struct PersistentHostOfferAdoptionInput {
    bool remote_description_ever_applied = false;
    std::string_view last_applied_host_instance_id;
    std::string_view offered_host_instance_id;
    bool exact_failed_offer = false;
    bool completed_offer = false;
    bool authenticated_once = true;
    std::uint64_t current_generation = 0;
    std::uint64_t offered_generation = 0;
};

enum class EstablishedSessionRecoveryAction {
    kNone,
    kWaitGrace,
    kRecover,
};

struct EstablishedSessionRecoveryInput {
    bool connected_once = false;
    bool transport_failed = false;
    std::uint64_t transport_failure_observed_at_ms = 0;
    std::uint64_t required_channel_closed_at_ms = 0;
    std::uint64_t now_ms = 0;
    std::uint64_t grace_ms = 0;
};

// Once a session has connected, an ICE Failed callback or required-channel
// close is owned by automatic recovery immediately. The grace period delays
// rebuilding; it must never expose the failure to the generic fatal path.
[[nodiscard]] EstablishedSessionRecoveryAction decide_established_session_recovery(
    const EstablishedSessionRecoveryInput& input) noexcept;

// A failed libdatachannel transport cannot be revived by trickling additional
// candidates into it. After bounded backoff, build a fresh Controller request
// without waiting for obsolete candidates/publication/ACK. Only the Host may
// create the replacement generation in response to that correlated request.
// Initial network failure is not terminal after a fixed attempt count: keep
// retrying with capped backoff while the owning runtime remains active.
[[nodiscard]] DhtIceFailureRecoveryDecision decide_dht_ice_failure_recovery(
    const DhtIceFailureRecoveryInput& input) noexcept;

// A Controller recovery request must not re-adopt the exact Host offer whose
// transport already failed. Generation numbers are process-local and can reset
// when the Host restarts, so the SDP-derived offer tag is the durable identity.
[[nodiscard]] bool should_skip_failed_dht_host_offer(
    std::uint64_t remote_generation,
    std::uint64_t failed_generation,
    std::string_view remote_offer_tag,
    std::string_view failed_offer_tag) noexcept;

// A Host record that already acknowledges a Controller answer describes a
// completed negotiation. A newly started Controller has no process-local
// failed-generation history, but it still must not create another answer for
// those already-consumed ICE credentials.
[[nodiscard]] bool should_skip_completed_dht_host_offer_on_fresh_controller(
    std::uint64_t controller_generation,
    std::string_view acknowledged_answer_tag) noexcept;

// A mismatched persistent offer may be adopted only by a fresh Controller or
// when a cryptographically random publisher identity proves that the Host
// process restarted. Same-process recovery keeps the request-tag handshake.
[[nodiscard]] PersistentHostOfferAdoption decide_persistent_host_offer_adoption(
    const PersistentHostOfferAdoptionInput& input) noexcept;
[[nodiscard]] std::string persistent_host_offer_adoption_to_string(
    PersistentHostOfferAdoption adoption);

// The initial Controller wake record must not replace a Host's first standby
// offer. Once any prior peer description has been applied (or ICE has failed),
// a fresh Controller request owns recovery and must replace the stale standby
// generation after that attempt/session ends.
// A stale/expired publication cannot justify indefinitely ignoring a new request.
[[nodiscard]] bool should_defer_dht_controller_request_for_persistent_offer(
    bool persistent_offer_active,
    bool transport_failed,
    bool remote_description_applied,
    bool remote_description_ever_applied,
    bool standby_publication_viable) noexcept;

// A Host that is rebuilding because it just accepted a newer Controller
// request must correlate the replacement offer with that request. Falling
// back to a process-local standby tag makes the durable peers describe
// different attempts and can leave both sides waiting after the first ICE
// failure.
[[nodiscard]] std::string select_dht_host_rebuild_request_tag(
    bool accepted_controller_request,
    std::string_view accepted_controller_request_tag,
    std::string_view standby_request_tag);

// The initial unattended Host publishes a standby offer. After a connected
// session is lost, however, the Controller's fresh request owns recovery; an
// extra uncorrelated standby offer costs another DHT round and can race the
// matching generation.
[[nodiscard]] bool should_defer_dht_host_rebuild_until_controller_request(
    bool post_connected_recovery,
    std::string_view accepted_controller_request_tag) noexcept;

// Owns the correlation policy for one signaling/ICE negotiation. It deliberately
// has no clocks: delivery timeouts may request retransmission, but only a Host
// offer is allowed to advance the ICE generation.
class ConnectionNegotiationCoordinator final {
public:
    explicit ConnectionNegotiationCoordinator(ConnectionNegotiationRole role);

    [[nodiscard]] ConnectionNegotiationResult begin_controller_request(
        std::string_view connection_request_tag);
    [[nodiscard]] bool mark_controller_request_published();
    [[nodiscard]] ConnectionNegotiationResult observe_controller_request(
        std::string_view connection_request_tag);
    [[nodiscard]] ConnectionNegotiationResult begin_host_generation(
        std::uint64_t generation,
        std::string_view offer_description_tag);
    [[nodiscard]] ConnectionNegotiationResult observe_host_offer(
        std::uint64_t generation,
        std::string_view connection_request_tag,
        std::string_view offer_description_tag,
        PersistentHostOfferAdoption persistent_offer_adoption =
            PersistentHostOfferAdoption::kRejected);
    [[nodiscard]] bool mark_remote_offer_applied();
    [[nodiscard]] bool mark_local_answer_ready(std::string_view answer_description_tag);
    [[nodiscard]] bool mark_local_answer_published();
    [[nodiscard]] ConnectionNegotiationResult observe_controller_answer(
        std::uint64_t generation,
        std::string_view connection_request_tag,
        std::string_view answered_offer_description_tag,
        std::string_view answer_description_tag);
    [[nodiscard]] bool mark_remote_answer_applied(std::string_view answer_description_tag);
    [[nodiscard]] ConnectionNegotiationResult observe_answer_applied(
        std::uint64_t generation,
        std::string_view connection_request_tag,
        std::string_view offer_description_tag,
        std::string_view acknowledged_answer_description_tag);
    [[nodiscard]] bool mark_ice_connecting();
    [[nodiscard]] bool mark_connected();
    [[nodiscard]] bool mark_failed();
    [[nodiscard]] bool reset_for_reconnect(std::string_view controller_request_tag = {});

    // A delivery timeout never mutates the active generation. The caller may
    // retransmit the same cumulative envelope through any configured adapter.
    [[nodiscard]] ConnectionNegotiationResult observe_delivery_timeout() const noexcept;

    [[nodiscard]] bool local_answer_published() const noexcept;
    [[nodiscard]] bool answer_acknowledged() const noexcept;
    [[nodiscard]] bool local_timer_may_advance_generation() const noexcept;
    [[nodiscard]] ConnectionNegotiationRole role() const noexcept;
    [[nodiscard]] ConnectionNegotiationPhase phase() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] const std::string& connection_request_tag() const noexcept;
    [[nodiscard]] const std::string& offer_description_tag() const noexcept;
    [[nodiscard]] const std::string& answer_description_tag() const noexcept;
    [[nodiscard]] const std::string& acknowledged_answer_description_tag() const noexcept;
    // Remote ACK is receive-only at the Controller: never echo it in Answer.
    [[nodiscard]] std::string_view outbound_answer_acknowledgement() const noexcept;

private:
    void reset_generation_state(std::uint64_t generation, std::string_view offer_description_tag);

    ConnectionNegotiationRole role_;
    ConnectionNegotiationPhase phase_ = ConnectionNegotiationPhase::kIdle;
    std::uint64_t generation_ = 0;
    std::string connection_request_tag_;
    std::string offer_description_tag_;
    std::string answer_description_tag_;
    std::string acknowledged_answer_description_tag_;
    bool local_answer_published_ = false;
};

[[nodiscard]] std::string_view connection_negotiation_phase_to_string(
    ConnectionNegotiationPhase phase) noexcept;
[[nodiscard]] std::string_view connection_negotiation_observation_to_string(
    ConnectionNegotiationObservation observation) noexcept;
[[nodiscard]] std::string_view dht_ice_failure_recovery_action_to_string(
    DhtIceFailureRecoveryAction action) noexcept;
[[nodiscard]] std::uint32_t automatic_reconnect_backoff_ms(
    std::uint32_t attempt_index) noexcept;

}  // namespace redclaw::service
