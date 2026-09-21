#include "redclaw/service/connection_negotiation.h"

#include "redclaw/service/dht_rendezvous.h"

#include <algorithm>
#include <array>
#include <limits>

namespace redclaw::service {

namespace {

constexpr std::array<std::uint32_t, 5> kAutomaticReconnectBackoffMs = {
    1000, 2000, 4000, 8000, 16000};

ConnectionNegotiationResult result(
    ConnectionNegotiationObservation observation,
    bool generation_changed = false,
    bool state_changed = false) {
    return ConnectionNegotiationResult {
        .observation = observation,
        .generation_changed = generation_changed,
        .state_changed = state_changed,
    };
}

}  // namespace

std::uint32_t automatic_reconnect_backoff_ms(
    std::uint32_t attempt_index) noexcept {
    const auto bounded_index = std::min<std::size_t>(
        attempt_index,
        kAutomaticReconnectBackoffMs.size() - 1);
    return kAutomaticReconnectBackoffMs[bounded_index];
}

DhtIceFailureRecoveryDecision decide_dht_ice_failure_recovery(
    const DhtIceFailureRecoveryInput& input) noexcept {
    if (!input.transport_failed) {
        return {.may_apply_remote_candidates = input.remote_description_applied};
    }

    DhtIceFailureRecoveryDecision decision;
    decision.may_apply_remote_candidates = false;
    if (input.connected_once || input.automatic_recovery_owned) {
        // The established-session reconnect policy owns this case.
        return decision;
    }
    if (input.role == ConnectionNegotiationRole::kHost) {
        decision.action = DhtIceFailureRecoveryAction::kWaitForHostRepair;
        return decision;
    }
    if (input.failure_elapsed_ms < automatic_reconnect_backoff_ms(input.repair_attempts)) {
        decision.action = DhtIceFailureRecoveryAction::kWaitBackoff;
        return decision;
    }

    decision.action = DhtIceFailureRecoveryAction::kRebuildControllerRequest;
    return decision;
}

AutomaticReconnectAction AutomaticReconnectSchedule::tick(
    std::uint64_t now_ms, std::uint64_t stage_window_ms, bool transport_failed) {
    if (!active || stage_window_ms == 0) {
        return AutomaticReconnectAction::kNone;
    }
    if (in_flight && (transport_failed || now_ms >= deadline_ms)) {
        in_flight = false;
        deadline_ms = 0;
        retry_after_ms = now_ms + automatic_reconnect_backoff_ms(attempt_count);
        return AutomaticReconnectAction::kAttemptFailed;
    }
    if (in_flight) {
        return AutomaticReconnectAction::kNone;
    }
    if (retry_after_ms == 0) {
        retry_after_ms = now_ms + automatic_reconnect_backoff_ms(attempt_count);
        return AutomaticReconnectAction::kScheduled;
    }
    if (now_ms < retry_after_ms) {
        return AutomaticReconnectAction::kNone;
    }
    if (attempt_count < std::numeric_limits<std::uint32_t>::max()) {
        ++attempt_count;
    }
    retry_after_ms = 0;
    in_flight = true;
    deadline_ms = now_ms + stage_window_ms;
    total_deadline_ms = now_ms + stage_window_ms * 4;
    phase = ConnectionNegotiationPhase::kRequestReady;
    generation = 0;
    return AutomaticReconnectAction::kRebuild;
}

bool AutomaticReconnectSchedule::observe_progress(
    ConnectionNegotiationPhase next_phase, std::uint64_t next_generation,
    std::uint64_t now_ms, std::uint64_t stage_window_ms) {
    if (!active || !in_flight || now_ms >= deadline_ms
        || next_generation == 0 || (generation != 0 && next_generation != generation)
        || next_phase <= phase || next_phase >= ConnectionNegotiationPhase::kConnected) {
        return false;
    }
    generation = next_generation;
    phase = next_phase;
    deadline_ms = std::min(total_deadline_ms, now_ms + stage_window_ms);
    return true;
}

bool should_skip_failed_dht_host_offer(
    std::uint64_t remote_generation,
    std::uint64_t failed_generation,
    std::string_view remote_offer_tag,
    std::string_view failed_offer_tag) noexcept {
    return remote_generation != 0
        && failed_generation != 0
        && remote_generation <= failed_generation
        && !remote_offer_tag.empty()
        && remote_offer_tag == failed_offer_tag;
}

bool should_skip_completed_dht_host_offer_on_fresh_controller(
    std::uint64_t controller_generation,
    std::string_view acknowledged_answer_tag) noexcept {
    return controller_generation == 0
        && !acknowledged_answer_tag.empty()
        && acknowledged_answer_tag != "none";
}

bool should_defer_dht_controller_request_for_persistent_offer(
    bool persistent_offer_active,
    bool transport_failed,
    bool remote_description_applied,
    bool remote_description_ever_applied,
    bool standby_publication_viable) noexcept {
    return persistent_offer_active
        && standby_publication_viable
        && !transport_failed
        && !remote_description_applied
        && !remote_description_ever_applied;
}

PersistentHostOfferAdoption decide_persistent_host_offer_adoption(
    const PersistentHostOfferAdoptionInput& input) noexcept {
    if (input.exact_failed_offer || input.completed_offer) {
        return PersistentHostOfferAdoption::kRejected;
    }
    if (!input.remote_description_ever_applied) {
        return PersistentHostOfferAdoption::kFreshController;
    }
    if (is_valid_dht_publisher_instance_id(input.last_applied_host_instance_id)
        && is_valid_dht_publisher_instance_id(input.offered_host_instance_id)
        && input.last_applied_host_instance_id != input.offered_host_instance_id) {
        return PersistentHostOfferAdoption::kRestartedHost;
    }
    if (!input.authenticated_once && input.current_generation > 0
        && input.offered_generation > input.current_generation
        && is_valid_dht_publisher_instance_id(input.last_applied_host_instance_id)
        && input.last_applied_host_instance_id == input.offered_host_instance_id) {
        return PersistentHostOfferAdoption::kNewerUnauthenticatedHost;
    }
    return PersistentHostOfferAdoption::kRejected;
}

std::string persistent_host_offer_adoption_to_string(
    PersistentHostOfferAdoption adoption) {
    switch (adoption) {
    case PersistentHostOfferAdoption::kRejected:
        return "rejected";
    case PersistentHostOfferAdoption::kFreshController:
        return "fresh_controller";
    case PersistentHostOfferAdoption::kRestartedHost:
        return "restarted_host";
    case PersistentHostOfferAdoption::kNewerUnauthenticatedHost:
        return "newer_unauthenticated_host";
    }
    return "rejected";
}

std::string select_dht_host_rebuild_request_tag(
    bool accepted_controller_request,
    std::string_view accepted_controller_request_tag,
    std::string_view standby_request_tag) {
    if (accepted_controller_request && !accepted_controller_request_tag.empty()) {
        return std::string(accepted_controller_request_tag);
    }
    return std::string(standby_request_tag);
}

bool should_defer_dht_host_rebuild_until_controller_request(
    bool post_connected_recovery,
    std::string_view accepted_controller_request_tag) noexcept {
    return post_connected_recovery && accepted_controller_request_tag.empty();
}

EstablishedSessionRecoveryAction decide_established_session_recovery(
    const EstablishedSessionRecoveryInput& input) noexcept {
    if (!input.connected_once) {
        return EstablishedSessionRecoveryAction::kNone;
    }
    std::uint64_t failure_at_ms = 0;
    if (input.transport_failed) {
        failure_at_ms = input.transport_failure_observed_at_ms;
    }
    if (input.required_channel_closed_at_ms != 0
        && (failure_at_ms == 0 || input.required_channel_closed_at_ms < failure_at_ms)) {
        failure_at_ms = input.required_channel_closed_at_ms;
    }
    if (failure_at_ms == 0) {
        return EstablishedSessionRecoveryAction::kNone;
    }
    if (input.now_ms < failure_at_ms
        || input.now_ms - failure_at_ms < input.grace_ms) {
        return EstablishedSessionRecoveryAction::kWaitGrace;
    }
    return EstablishedSessionRecoveryAction::kRecover;
}

ConnectionNegotiationCoordinator::ConnectionNegotiationCoordinator(ConnectionNegotiationRole role)
    : role_(role) {}

ConnectionNegotiationResult ConnectionNegotiationCoordinator::begin_controller_request(
    std::string_view connection_request_tag) {
    if (role_ != ConnectionNegotiationRole::kController) {
        return result(ConnectionNegotiationObservation::kRejectedRole);
    }
    if (connection_request_tag.empty()) {
        return result(ConnectionNegotiationObservation::kRejectedInvalid);
    }
    if (!connection_request_tag_.empty()) {
        if (connection_request_tag_ == connection_request_tag) {
            return result(ConnectionNegotiationObservation::kDuplicate);
        }
        return result(ConnectionNegotiationObservation::kRejectedGenerationConflict);
    }
    connection_request_tag_ = connection_request_tag;
    phase_ = ConnectionNegotiationPhase::kRequestReady;
    return result(ConnectionNegotiationObservation::kAccepted, false, true);
}

bool ConnectionNegotiationCoordinator::mark_controller_request_published() {
    if (role_ != ConnectionNegotiationRole::kController
        || phase_ != ConnectionNegotiationPhase::kRequestReady
        || connection_request_tag_.empty()) {
        return false;
    }
    phase_ = ConnectionNegotiationPhase::kRequestPublished;
    return true;
}

ConnectionNegotiationResult ConnectionNegotiationCoordinator::observe_controller_request(
    std::string_view connection_request_tag) {
    if (role_ != ConnectionNegotiationRole::kHost) {
        return result(ConnectionNegotiationObservation::kRejectedRole);
    }
    if (connection_request_tag.empty()) {
        return result(ConnectionNegotiationObservation::kRejectedInvalid);
    }
    if (!connection_request_tag_.empty()) {
        if (connection_request_tag_ == connection_request_tag) {
            return result(ConnectionNegotiationObservation::kDuplicate);
        }
        if (phase_ == ConnectionNegotiationPhase::kConnected) {
            return result(ConnectionNegotiationObservation::kRejectedGenerationConflict);
        }

        // The Controller lane is a latest-state register, not a message queue.
        // A different authenticated request tag therefore supersedes any
        // unconnected attempt that was bound to the previous Controller
        // process. Keeping the old tag would make a long-running Host reject a
        // restarted Controller forever.
        const bool replaced_active_generation = generation_ != 0;
        generation_ = 0;
        connection_request_tag_ = connection_request_tag;
        offer_description_tag_.clear();
        answer_description_tag_.clear();
        acknowledged_answer_description_tag_.clear();
        local_answer_published_ = false;
        phase_ = ConnectionNegotiationPhase::kRequestReceived;
        return result(
            ConnectionNegotiationObservation::kAccepted,
            replaced_active_generation,
            true);
    }
    connection_request_tag_ = connection_request_tag;
    offer_description_tag_.clear();
    answer_description_tag_.clear();
    acknowledged_answer_description_tag_.clear();
    local_answer_published_ = false;
    phase_ = ConnectionNegotiationPhase::kRequestReceived;
    return result(ConnectionNegotiationObservation::kAccepted, false, true);
}

void ConnectionNegotiationCoordinator::reset_generation_state(
    std::uint64_t generation,
    std::string_view offer_description_tag) {
    generation_ = generation;
    offer_description_tag_ = offer_description_tag;
    answer_description_tag_.clear();
    acknowledged_answer_description_tag_.clear();
    local_answer_published_ = false;
    phase_ = ConnectionNegotiationPhase::kOfferReady;
}

ConnectionNegotiationResult ConnectionNegotiationCoordinator::begin_host_generation(
    std::uint64_t generation,
    std::string_view offer_description_tag) {
    if (role_ != ConnectionNegotiationRole::kHost) {
        return result(ConnectionNegotiationObservation::kRejectedRole);
    }
    if (generation == 0 || offer_description_tag.empty()) {
        return result(ConnectionNegotiationObservation::kRejectedInvalid);
    }
    if (connection_request_tag_.empty()) {
        return result(ConnectionNegotiationObservation::kRejectedInvalid);
    }
    if (generation < generation_) {
        return result(ConnectionNegotiationObservation::kRejectedStaleGeneration);
    }
    if (generation == generation_) {
        if (offer_description_tag_ == offer_description_tag) {
            return result(ConnectionNegotiationObservation::kDuplicate);
        }
        return result(ConnectionNegotiationObservation::kRejectedGenerationConflict);
    }

    reset_generation_state(generation, offer_description_tag);
    return result(ConnectionNegotiationObservation::kAccepted, true, true);
}

ConnectionNegotiationResult ConnectionNegotiationCoordinator::observe_host_offer(
    std::uint64_t generation,
    std::string_view connection_request_tag,
    std::string_view offer_description_tag,
    PersistentHostOfferAdoption persistent_offer_adoption) {
    if (role_ != ConnectionNegotiationRole::kController) {
        return result(ConnectionNegotiationObservation::kRejectedRole);
    }
    if (generation == 0 || connection_request_tag.empty() || offer_description_tag.empty()) {
        return result(ConnectionNegotiationObservation::kRejectedInvalid);
    }
    if (connection_request_tag != connection_request_tag_) {
        const bool may_adopt_persistent_host_offer =
            persistent_offer_adoption != PersistentHostOfferAdoption::kRejected
            && generation_ == 0
            && (phase_ == ConnectionNegotiationPhase::kRequestReady
                || phase_ == ConnectionNegotiationPhase::kRequestPublished);
        const bool may_replace_unauthenticated_offer =
            persistent_offer_adoption == PersistentHostOfferAdoption::kNewerUnauthenticatedHost
            && generation_ > 0 && generation > generation_;
        if (!may_adopt_persistent_host_offer && !may_replace_unauthenticated_offer) {
            return result(ConnectionNegotiationObservation::kRejectedOfferMismatch);
        }
        // A long-running Host publishes its complete standby offer before a
        // Controller appears. The Controller request is only a wake/repair
        // record; the first valid Host offer becomes the correlation lease.
        connection_request_tag_ = std::string(connection_request_tag);
    }
    if (generation < generation_) {
        return result(ConnectionNegotiationObservation::kRejectedStaleGeneration);
    }
    if (generation == generation_) {
        if (offer_description_tag_ == offer_description_tag) {
            return result(ConnectionNegotiationObservation::kDuplicate);
        }
        return result(ConnectionNegotiationObservation::kRejectedGenerationConflict);
    }

    reset_generation_state(generation, offer_description_tag);
    return result(ConnectionNegotiationObservation::kAccepted, true, true);
}

bool ConnectionNegotiationCoordinator::mark_remote_offer_applied() {
    if (role_ != ConnectionNegotiationRole::kController
        || generation_ == 0
        || offer_description_tag_.empty()) {
        return false;
    }

    switch (phase_) {
    case ConnectionNegotiationPhase::kOfferReady:
        phase_ = ConnectionNegotiationPhase::kOfferApplied;
        return true;
    case ConnectionNegotiationPhase::kOfferApplied:
    case ConnectionNegotiationPhase::kAnswerReady:
    case ConnectionNegotiationPhase::kAnswerPublished:
    case ConnectionNegotiationPhase::kAnswerApplied:
    case ConnectionNegotiationPhase::kIceConnecting:
    case ConnectionNegotiationPhase::kConnected:
    case ConnectionNegotiationPhase::kFailed:
        // libdatachannel may synchronously emit the local answer while the
        // remote offer call is still on the stack. Never move that callback-
        // advanced state backwards to offer_applied.
        return true;
    default:
        return false;
    }
}

bool ConnectionNegotiationCoordinator::mark_local_answer_ready(std::string_view answer_description_tag) {
    if (role_ != ConnectionNegotiationRole::kController
        || generation_ == 0
        || offer_description_tag_.empty()
        || answer_description_tag.empty()
        || phase_ == ConnectionNegotiationPhase::kFailed) {
        return false;
    }
    if (!answer_description_tag_.empty()) {
        return answer_description_tag_ == answer_description_tag;
    }
    answer_description_tag_ = answer_description_tag;
    acknowledged_answer_description_tag_.clear();
    local_answer_published_ = false;
    phase_ = ConnectionNegotiationPhase::kAnswerReady;
    return true;
}

bool ConnectionNegotiationCoordinator::mark_local_answer_published() {
    if (role_ != ConnectionNegotiationRole::kController
        || answer_description_tag_.empty()
        || phase_ == ConnectionNegotiationPhase::kFailed) {
        return false;
    }
    if (phase_ == ConnectionNegotiationPhase::kAnswerReady) {
        phase_ = ConnectionNegotiationPhase::kAnswerPublished;
    }
    local_answer_published_ = true;
    return true;
}

ConnectionNegotiationResult ConnectionNegotiationCoordinator::observe_controller_answer(
    std::uint64_t generation,
    std::string_view connection_request_tag,
    std::string_view answered_offer_description_tag,
    std::string_view answer_description_tag) {
    if (role_ != ConnectionNegotiationRole::kHost) {
        return result(ConnectionNegotiationObservation::kRejectedRole);
    }
    if (generation == 0 || connection_request_tag.empty()
        || answered_offer_description_tag.empty() || answer_description_tag.empty()) {
        return result(ConnectionNegotiationObservation::kRejectedInvalid);
    }
    if (connection_request_tag != connection_request_tag_) {
        return result(ConnectionNegotiationObservation::kRejectedOfferMismatch);
    }
    if (generation < generation_) {
        return result(ConnectionNegotiationObservation::kRejectedStaleGeneration);
    }
    if (generation > generation_) {
        return result(ConnectionNegotiationObservation::kRejectedGenerationConflict);
    }
    if (answered_offer_description_tag != offer_description_tag_) {
        return result(ConnectionNegotiationObservation::kRejectedOfferMismatch);
    }
    if (phase_ == ConnectionNegotiationPhase::kFailed) {
        return result(ConnectionNegotiationObservation::kRejectedTerminalState);
    }
    if (!answer_description_tag_.empty()) {
        if (answer_description_tag_ == answer_description_tag) {
            return result(ConnectionNegotiationObservation::kDuplicate);
        }
        return result(ConnectionNegotiationObservation::kRejectedAnswerMismatch);
    }

    answer_description_tag_ = answer_description_tag;
    phase_ = ConnectionNegotiationPhase::kAnswerReady;
    return result(ConnectionNegotiationObservation::kAccepted, false, true);
}

bool ConnectionNegotiationCoordinator::mark_remote_answer_applied(
    std::string_view answer_description_tag) {
    if (role_ != ConnectionNegotiationRole::kHost
        || answer_description_tag_.empty()
        || answer_description_tag != answer_description_tag_
        || phase_ == ConnectionNegotiationPhase::kFailed) {
        return false;
    }
    acknowledged_answer_description_tag_ = answer_description_tag;
    if (phase_ < ConnectionNegotiationPhase::kAnswerApplied) {
        phase_ = ConnectionNegotiationPhase::kAnswerApplied;
    }
    return true;
}

ConnectionNegotiationResult ConnectionNegotiationCoordinator::observe_answer_applied(
    std::uint64_t generation,
    std::string_view connection_request_tag,
    std::string_view offer_description_tag,
    std::string_view acknowledged_answer_description_tag) {
    if (role_ != ConnectionNegotiationRole::kController) {
        return result(ConnectionNegotiationObservation::kRejectedRole);
    }
    if (generation == 0 || connection_request_tag.empty()
        || offer_description_tag.empty() || acknowledged_answer_description_tag.empty()) {
        return result(ConnectionNegotiationObservation::kRejectedInvalid);
    }
    if (connection_request_tag != connection_request_tag_) {
        return result(ConnectionNegotiationObservation::kRejectedOfferMismatch);
    }
    if (generation < generation_) {
        return result(ConnectionNegotiationObservation::kRejectedStaleGeneration);
    }
    if (generation > generation_) {
        return result(ConnectionNegotiationObservation::kRejectedGenerationConflict);
    }
    if (offer_description_tag != offer_description_tag_) {
        return result(ConnectionNegotiationObservation::kRejectedOfferMismatch);
    }
    if (answer_description_tag_.empty()
        || acknowledged_answer_description_tag != answer_description_tag_) {
        return result(ConnectionNegotiationObservation::kRejectedAnswerMismatch);
    }
    if (phase_ == ConnectionNegotiationPhase::kFailed) {
        return result(ConnectionNegotiationObservation::kRejectedTerminalState);
    }
    if (acknowledged_answer_description_tag_ == acknowledged_answer_description_tag) {
        return result(ConnectionNegotiationObservation::kDuplicate);
    }

    acknowledged_answer_description_tag_ = acknowledged_answer_description_tag;
    if (phase_ < ConnectionNegotiationPhase::kAnswerApplied) {
        phase_ = ConnectionNegotiationPhase::kAnswerApplied;
    }
    return result(ConnectionNegotiationObservation::kAccepted, false, true);
}

bool ConnectionNegotiationCoordinator::mark_ice_connecting() {
    if (answer_description_tag_.empty() || phase_ == ConnectionNegotiationPhase::kFailed) {
        return false;
    }
    if (phase_ < ConnectionNegotiationPhase::kIceConnecting) {
        phase_ = ConnectionNegotiationPhase::kIceConnecting;
    }
    return true;
}

bool ConnectionNegotiationCoordinator::mark_connected() {
    if (answer_description_tag_.empty() || phase_ == ConnectionNegotiationPhase::kFailed) {
        return false;
    }
    phase_ = ConnectionNegotiationPhase::kConnected;
    return true;
}

bool ConnectionNegotiationCoordinator::mark_failed() {
    if (generation_ == 0) {
        return false;
    }
    phase_ = ConnectionNegotiationPhase::kFailed;
    return true;
}

bool ConnectionNegotiationCoordinator::reset_for_reconnect(std::string_view controller_request_tag) {
    if (role_ == ConnectionNegotiationRole::kController && controller_request_tag.empty()) {
        return false;
    }
    generation_ = 0;
    connection_request_tag_.clear();
    offer_description_tag_.clear();
    answer_description_tag_.clear();
    acknowledged_answer_description_tag_.clear();
    local_answer_published_ = false;
    phase_ = ConnectionNegotiationPhase::kIdle;
    if (role_ == ConnectionNegotiationRole::kController) {
        return begin_controller_request(controller_request_tag).accepted();
    }
    return true;
}

ConnectionNegotiationResult ConnectionNegotiationCoordinator::observe_delivery_timeout() const noexcept {
    return result(ConnectionNegotiationObservation::kDuplicate);
}

bool ConnectionNegotiationCoordinator::local_answer_published() const noexcept {
    return local_answer_published_;
}

bool ConnectionNegotiationCoordinator::answer_acknowledged() const noexcept {
    return !answer_description_tag_.empty()
        && acknowledged_answer_description_tag_ == answer_description_tag_;
}

bool ConnectionNegotiationCoordinator::local_timer_may_advance_generation() const noexcept {
    return role_ == ConnectionNegotiationRole::kHost;
}

ConnectionNegotiationRole ConnectionNegotiationCoordinator::role() const noexcept {
    return role_;
}

ConnectionNegotiationPhase ConnectionNegotiationCoordinator::phase() const noexcept {
    return phase_;
}

std::uint64_t ConnectionNegotiationCoordinator::generation() const noexcept {
    return generation_;
}

const std::string& ConnectionNegotiationCoordinator::connection_request_tag() const noexcept {
    return connection_request_tag_;
}

const std::string& ConnectionNegotiationCoordinator::offer_description_tag() const noexcept {
    return offer_description_tag_;
}

const std::string& ConnectionNegotiationCoordinator::answer_description_tag() const noexcept {
    return answer_description_tag_;
}

const std::string& ConnectionNegotiationCoordinator::acknowledged_answer_description_tag() const noexcept {
    return acknowledged_answer_description_tag_;
}

std::string_view ConnectionNegotiationCoordinator::outbound_answer_acknowledgement() const noexcept {
    return role_ == ConnectionNegotiationRole::kHost
        ? std::string_view(acknowledged_answer_description_tag_) : std::string_view{};
}

std::string_view connection_negotiation_phase_to_string(ConnectionNegotiationPhase phase) noexcept {
    switch (phase) {
    case ConnectionNegotiationPhase::kIdle:
        return "idle";
    case ConnectionNegotiationPhase::kRequestReady:
        return "request_ready";
    case ConnectionNegotiationPhase::kRequestPublished:
        return "request_published";
    case ConnectionNegotiationPhase::kRequestReceived:
        return "request_received";
    case ConnectionNegotiationPhase::kOfferReady:
        return "offer_ready";
    case ConnectionNegotiationPhase::kOfferApplied:
        return "offer_applied";
    case ConnectionNegotiationPhase::kAnswerReady:
        return "answer_ready";
    case ConnectionNegotiationPhase::kAnswerPublished:
        return "answer_published";
    case ConnectionNegotiationPhase::kAnswerApplied:
        return "answer_applied";
    case ConnectionNegotiationPhase::kIceConnecting:
        return "ice_connecting";
    case ConnectionNegotiationPhase::kConnected:
        return "connected";
    case ConnectionNegotiationPhase::kFailed:
        return "failed";
    }
    return "unknown";
}

std::string_view connection_negotiation_observation_to_string(
    ConnectionNegotiationObservation observation) noexcept {
    switch (observation) {
    case ConnectionNegotiationObservation::kAccepted:
        return "accepted";
    case ConnectionNegotiationObservation::kDuplicate:
        return "duplicate";
    case ConnectionNegotiationObservation::kRejectedRole:
        return "rejected_role";
    case ConnectionNegotiationObservation::kRejectedInvalid:
        return "rejected_invalid";
    case ConnectionNegotiationObservation::kRejectedStaleGeneration:
        return "rejected_stale_generation";
    case ConnectionNegotiationObservation::kRejectedGenerationConflict:
        return "rejected_generation_conflict";
    case ConnectionNegotiationObservation::kRejectedOfferMismatch:
        return "rejected_offer_mismatch";
    case ConnectionNegotiationObservation::kRejectedAnswerMismatch:
        return "rejected_answer_mismatch";
    case ConnectionNegotiationObservation::kRejectedTerminalState:
        return "rejected_terminal_state";
    }
    return "unknown";
}

std::string_view dht_ice_failure_recovery_action_to_string(
    DhtIceFailureRecoveryAction action) noexcept {
    switch (action) {
    case DhtIceFailureRecoveryAction::kNone:
        return "none";
    case DhtIceFailureRecoveryAction::kWaitBackoff:
        return "wait_backoff";
    case DhtIceFailureRecoveryAction::kRebuildControllerRequest:
        return "rebuild_controller_request";
    case DhtIceFailureRecoveryAction::kWaitForHostRepair:
        return "wait_for_host_repair";
    }
    return "unknown";
}

}  // namespace redclaw::service
