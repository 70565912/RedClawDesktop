#include "redclaw/session/session_module.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace redclaw::session {

namespace {

constexpr std::uint32_t work_reason_mask(HostStreamWorkReason reason) {
    return static_cast<std::uint32_t>(reason);
}

} // namespace

DesktopSourceActivityTracker::DesktopSourceActivityTracker(DesktopSourceActivityConfig config)
    : config_(config) {
    config_.static_quiet_ms = std::max<std::uint64_t>(1, config_.static_quiet_ms);
    config_.capture_stall_ms = std::max<std::uint64_t>(1, config_.capture_stall_ms);
    config_.minimum_timeout_polls = std::max<std::uint32_t>(1, config_.minimum_timeout_polls);
}

void DesktopSourceActivityTracker::reset(
    std::uint64_t stream_geometry_revision,
    std::uint64_t rate_revision) {
    snapshot_ = {};
    snapshot_.stream_geometry_revision = stream_geometry_revision;
    snapshot_.rate_revision = rate_revision;
    current_poll_started_ms_ = 0;
    reference_last_attempt_ms_ = 0;
    reference_attempt_ = 0;
    capture_stall_reported_ = false;
}

void DesktopSourceActivityTracker::on_capture_poll_started(std::uint64_t now_ms) {
    ++snapshot_.capture_poll_started_total;
    current_poll_started_ms_ = now_ms;
    capture_stall_reported_ = false;
}

DesktopSourceActivityUpdate DesktopSourceActivityTracker::on_capture_timeout(std::uint64_t now_ms) {
    ++snapshot_.capture_poll_completed_total;
    ++snapshot_.capture_timeout_total;
    ++snapshot_.consecutive_timeouts;
    snapshot_.last_poll_completed_ms = now_ms;
    current_poll_started_ms_ = 0;
    capture_stall_reported_ = false;
    if (snapshot_.state == DesktopSourceActivityState::kActive && snapshot_.last_frame_ms != 0 &&
        now_ms >= snapshot_.last_frame_ms + config_.static_quiet_ms &&
        snapshot_.consecutive_timeouts >= config_.minimum_timeout_polls) {
        return enter_static_pending(true);
    }
    return update(false, false, false);
}

DesktopSourceActivityUpdate DesktopSourceActivityTracker::on_capture_frame(
    std::uint64_t now_ms,
    std::uint64_t stream_geometry_revision,
    std::uint64_t rate_revision) {
    ++snapshot_.capture_poll_completed_total;
    snapshot_.last_poll_completed_ms = now_ms;
    snapshot_.last_frame_ms = now_ms;
    snapshot_.consecutive_timeouts = 0;
    snapshot_.stream_geometry_revision = stream_geometry_revision;
    snapshot_.rate_revision = rate_revision;
    current_poll_started_ms_ = 0;
    capture_stall_reported_ = false;
    const auto previous = snapshot_.state;
    if (previous != DesktopSourceActivityState::kActive) {
        ++snapshot_.revision;
        if (previous == DesktopSourceActivityState::kStatic ||
            previous == DesktopSourceActivityState::kStaticPending) {
            ++snapshot_.static_exit_total;
        }
        snapshot_.state = DesktopSourceActivityState::kActive;
        snapshot_.reference_frame_id = 0;
        snapshot_.reference_keyframe_id = 0;
        reference_last_attempt_ms_ = 0;
        reference_attempt_ = 0;
        // Silence does not break the decoder's reference chain. First capture
        // and recovery from a failed/stalled source still require an IDR.
        return update(true, previous == DesktopSourceActivityState::kUnknown, false);
    }
    return update(false, false, false);
}

DesktopSourceActivityUpdate DesktopSourceActivityTracker::on_capture_failure(std::uint64_t now_ms) {
    ++snapshot_.capture_poll_completed_total;
    snapshot_.last_poll_completed_ms = now_ms;
    current_poll_started_ms_ = 0;
    capture_stall_reported_ = false;
    return enter_unknown(false);
}

DesktopSourceActivityUpdate DesktopSourceActivityTracker::tick(std::uint64_t now_ms) {
    const auto last_heartbeat = std::max(current_poll_started_ms_, snapshot_.last_poll_completed_ms);
    if (last_heartbeat != 0 && now_ms > last_heartbeat + config_.capture_stall_ms) {
        return enter_unknown(true);
    }
    return update(false, false, false);
}

DesktopSourceActivityUpdate DesktopSourceActivityTracker::on_viewport_change(
    std::uint64_t now_ms,
    std::uint64_t stream_geometry_revision,
    std::uint64_t rate_revision) {
    (void)now_ms;
    snapshot_.stream_geometry_revision = stream_geometry_revision;
    snapshot_.rate_revision = rate_revision;
    if (snapshot_.state == DesktopSourceActivityState::kStatic ||
        snapshot_.state == DesktopSourceActivityState::kStaticPending) {
        return enter_static_pending(true);
    }
    if (snapshot_.state == DesktopSourceActivityState::kActive) {
        return update(false, true, false);
    }
    return update(false, false, false);
}

DesktopSourceActivityUpdate DesktopSourceActivityTracker::on_reference_submitted(
    std::uint64_t frame_id,
    std::uint64_t keyframe_id,
    std::uint64_t now_ms,
    std::uint64_t rate_revision) {
    if (snapshot_.state != DesktopSourceActivityState::kStaticPending || frame_id == 0 || keyframe_id == 0) {
        return update(false, false, false);
    }
    snapshot_.reference_frame_id = frame_id;
    snapshot_.reference_keyframe_id = keyframe_id;
    snapshot_.rate_revision = rate_revision;
    reference_last_attempt_ms_ = now_ms;
    return update(true, false, false);
}

DesktopSourceActivityUpdate DesktopSourceActivityTracker::on_displayable_ack(
    std::uint64_t source_activity_revision,
    std::uint64_t latest_displayable_keyframe_id) {
    if (snapshot_.state != DesktopSourceActivityState::kStaticPending ||
        source_activity_revision != snapshot_.revision || snapshot_.reference_keyframe_id == 0 ||
        latest_displayable_keyframe_id < snapshot_.reference_keyframe_id) {
        return update(false, false, false);
    }
    snapshot_.state = DesktopSourceActivityState::kStatic;
    ++snapshot_.static_entry_total;
    return update(true, false, false);
}

bool DesktopSourceActivityTracker::reference_retry_due(std::uint64_t now_ms, std::uint32_t srtt_ms) const {
    return snapshot_.state == DesktopSourceActivityState::kStaticPending &&
        snapshot_.reference_keyframe_id != 0 && reference_last_attempt_ms_ != 0 &&
        now_ms >= reference_last_attempt_ms_ + reference_retry_delay_ms(srtt_ms);
}

DesktopSourceActivityUpdate DesktopSourceActivityTracker::mark_reference_retry(std::uint64_t now_ms) {
    if (snapshot_.state != DesktopSourceActivityState::kStaticPending) {
        return update(false, false, false);
    }
    ++snapshot_.reference_retry_total;
    ++reference_attempt_;
    reference_last_attempt_ms_ = now_ms;
    snapshot_.reference_frame_id = 0;
    snapshot_.reference_keyframe_id = 0;
    return update(true, true, true);
}

DesktopSourceActivitySnapshot DesktopSourceActivityTracker::snapshot() const {
    return snapshot_;
}

DesktopSourceActivityUpdate DesktopSourceActivityTracker::update(
    bool changed,
    bool request_keyframe,
    bool request_reference) const {
    return DesktopSourceActivityUpdate {
        .state_changed = changed,
        .request_keyframe = request_keyframe,
        .request_reference_frame = request_reference,
        .snapshot = snapshot_,
    };
}

DesktopSourceActivityUpdate DesktopSourceActivityTracker::enter_unknown(bool stalled) {
    if (snapshot_.state == DesktopSourceActivityState::kUnknown) {
        if (stalled && !capture_stall_reported_) {
            capture_stall_reported_ = true;
            ++snapshot_.capture_stall_total;
            if (snapshot_.revision == 0) {
                ++snapshot_.revision;
                return update(true, false, false);
            }
        }
        return update(false, false, false);
    }
    if (snapshot_.state == DesktopSourceActivityState::kStatic ||
        snapshot_.state == DesktopSourceActivityState::kStaticPending) {
        ++snapshot_.static_exit_total;
    }
    ++snapshot_.revision;
    snapshot_.state = DesktopSourceActivityState::kUnknown;
    snapshot_.reference_frame_id = 0;
    snapshot_.reference_keyframe_id = 0;
    reference_attempt_ = 0;
    reference_last_attempt_ms_ = 0;
    if (stalled) {
        ++snapshot_.capture_stall_total;
        capture_stall_reported_ = true;
    }
    return update(true, false, false);
}

DesktopSourceActivityUpdate DesktopSourceActivityTracker::enter_static_pending(bool request_keyframe) {
    ++snapshot_.revision;
    snapshot_.state = DesktopSourceActivityState::kStaticPending;
    snapshot_.reference_frame_id = 0;
    snapshot_.reference_keyframe_id = 0;
    reference_attempt_ = 0;
    reference_last_attempt_ms_ = 0;
    return update(true, request_keyframe, true);
}

std::uint64_t DesktopSourceActivityTracker::reference_retry_delay_ms(std::uint32_t srtt_ms) const {
    constexpr std::array<std::uint64_t, 4> retry_delays {1000, 2000, 4000, 5000};
    const auto index = std::min<std::size_t>(reference_attempt_, retry_delays.size() - 1);
    const auto network_floor = std::max<std::uint64_t>(1000, 2ULL * srtt_ms + 500ULL);
    return std::min<std::uint64_t>(5000, std::max(retry_delays[index], network_floor));
}

bool HostStreamWorkSnapshot::contains(HostStreamWorkReason reason) const {
    return (reasons & work_reason_mask(reason)) != 0;
}

std::chrono::milliseconds host_stream_encode_wait(
    const HostStreamEncodeReadiness& readiness, std::uint64_t now_ms) {
    if (!readiness.channels_ready || !readiness.source_active
        || !readiness.capacity_available || !readiness.frame_pending) {
        return std::chrono::milliseconds::max();
    }
    return std::chrono::milliseconds(readiness.due_ms > now_ms
        ? readiness.due_ms - now_ms : 0);
}

void HostStreamWorkCoordinator::post(HostStreamWorkReason reason) {
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        reasons_ |= work_reason_mask(reason);
    }
    changed_.notify_all();
}

HostStreamWorkSnapshot HostStreamWorkCoordinator::snapshot() const {
    std::lock_guard lock(mutex_);
    return {.generation = generation_, .reasons = reasons_};
}

HostStreamWorkSnapshot HostStreamWorkCoordinator::wait_for_change(
    std::uint64_t observed_generation,
    std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (timeout == std::chrono::milliseconds::max()) {
        changed_.wait(lock, [&] { return generation_ != observed_generation; });
    } else {
        changed_.wait_for(lock, timeout, [&] { return generation_ != observed_generation; });
    }
    return {.generation = generation_, .reasons = reasons_};
}

HostStreamWorkSnapshot HostStreamWorkCoordinator::consume(std::uint64_t observed_generation) {
    std::lock_guard lock(mutex_);
    if (generation_ == observed_generation) {
        return {.generation = generation_, .reasons = 0};
    }
    const HostStreamWorkSnapshot result {.generation = generation_, .reasons = reasons_};
    reasons_ = 0;
    return result;
}

void HostStreamWorkCoordinator::reset() {
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        reasons_ = 0;
    }
    changed_.notify_all();
}

ControllerDecoderRecoveryCoordinator::ControllerDecoderRecoveryCoordinator(
    ControllerDecoderRecoveryConfig config)
    : config_(config) {
    config_.minimum_retry_ms = std::max<std::uint64_t>(1, config_.minimum_retry_ms);
    config_.maximum_retry_ms = std::max(
        config_.minimum_retry_ms,
        config_.maximum_retry_ms);
}

void ControllerDecoderRecoveryCoordinator::reset() {
    std::lock_guard lock(mutex_);
    snapshot_ = {};
}

ControllerDecoderRecoveryUpdate ControllerDecoderRecoveryCoordinator::on_dependency_break(
    ControllerDecoderBreakReason reason,
    std::uint64_t frame_id,
    std::uint64_t now_ms,
    std::uint32_t srtt_ms) {
    std::lock_guard lock(mutex_);
    ++snapshot_.dependency_break_total;

    const std::uint32_t reason_mask = static_cast<std::uint32_t>(reason);
    if (snapshot_.state == ControllerDecoderRecoveryState::kAwaitingIdr) {
        snapshot_.reason_mask |= reason_mask;
        snapshot_.last_break_frame_id = std::max(snapshot_.last_break_frame_id, frame_id);
        ++snapshot_.suppressed_request_total;
        return update_locked(false, false);
    }

    if (snapshot_.state == ControllerDecoderRecoveryState::kAwaitingDisplayable) {
        ++snapshot_.invalidated_keyframe_total;
    }
    ++snapshot_.generation;
    snapshot_.state = ControllerDecoderRecoveryState::kAwaitingIdr;
    snapshot_.pending_keyframe_id = 0;
    snapshot_.last_break_frame_id = frame_id;
    snapshot_.last_request_ms = 0;
    snapshot_.retry_delay_ms = 0;
    snapshot_.request_attempt = 0;
    snapshot_.reason_mask = reason_mask;
    return request_keyframe_locked(now_ms, srtt_ms, false, true);
}

ControllerDecoderRecoveryUpdate ControllerDecoderRecoveryCoordinator::on_keyframe_submitted(
    std::uint64_t frame_id,
    std::uint64_t now_ms,
    std::uint32_t srtt_ms) {
    (void)now_ms;
    (void)srtt_ms;
    std::lock_guard lock(mutex_);
    if (frame_id == 0 || snapshot_.state == ControllerDecoderRecoveryState::kSynchronized) {
        return update_locked(false, false);
    }

    const bool state_changed =
        snapshot_.state != ControllerDecoderRecoveryState::kAwaitingDisplayable;
    snapshot_.state = ControllerDecoderRecoveryState::kAwaitingDisplayable;
    snapshot_.pending_keyframe_id = frame_id;
    return update_locked(state_changed, false);
}

ControllerDecoderRecoveryUpdate ControllerDecoderRecoveryCoordinator::on_displayable_ack(
    std::uint64_t latest_displayable_keyframe_id) {
    std::lock_guard lock(mutex_);
    if (snapshot_.state != ControllerDecoderRecoveryState::kAwaitingDisplayable
        || snapshot_.pending_keyframe_id == 0
        || latest_displayable_keyframe_id < snapshot_.pending_keyframe_id) {
        return update_locked(false, false);
    }

    snapshot_.state = ControllerDecoderRecoveryState::kSynchronized;
    snapshot_.pending_keyframe_id = 0;
    snapshot_.last_break_frame_id = 0;
    snapshot_.last_request_ms = 0;
    snapshot_.retry_delay_ms = 0;
    snapshot_.request_attempt = 0;
    snapshot_.reason_mask = 0;
    ++snapshot_.displayable_ack_total;
    return update_locked(true, false);
}

ControllerDecoderRecoveryUpdate ControllerDecoderRecoveryCoordinator::tick(
    std::uint64_t now_ms,
    std::uint32_t srtt_ms) {
    std::lock_guard lock(mutex_);
    if (snapshot_.state == ControllerDecoderRecoveryState::kSynchronized) {
        return update_locked(false, false);
    }
    if (snapshot_.last_request_ms == 0
        || now_ms >= snapshot_.last_request_ms + snapshot_.retry_delay_ms) {
        return request_keyframe_locked(now_ms, srtt_ms, true, false);
    }
    return update_locked(false, false);
}

bool ControllerDecoderRecoveryCoordinator::should_accept_frame(bool keyframe) const {
    std::lock_guard lock(mutex_);
    return snapshot_.state != ControllerDecoderRecoveryState::kAwaitingIdr || keyframe;
}

ControllerDecoderRecoverySnapshot ControllerDecoderRecoveryCoordinator::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

std::uint64_t ControllerDecoderRecoveryCoordinator::base_retry_delay_ms(
    std::uint32_t srtt_ms) const {
    const std::uint64_t network_delay = 2ULL * srtt_ms + config_.rtt_margin_ms;
    return std::clamp(
        network_delay,
        config_.minimum_retry_ms,
        config_.maximum_retry_ms);
}

ControllerDecoderRecoveryUpdate ControllerDecoderRecoveryCoordinator::request_keyframe_locked(
    std::uint64_t now_ms,
    std::uint32_t srtt_ms,
    bool retry,
    bool state_changed) {
    const std::uint64_t base_delay = base_retry_delay_ms(srtt_ms);
    const std::uint32_t shift = std::min<std::uint32_t>(snapshot_.request_attempt, 2);
    const std::uint64_t scaled_delay = base_delay << shift;
    snapshot_.retry_delay_ms = std::min(config_.maximum_retry_ms, scaled_delay);
    snapshot_.last_request_ms = now_ms;
    ++snapshot_.request_attempt;
    ++snapshot_.request_total;
    if (retry) {
        ++snapshot_.retry_total;
    }
    return update_locked(state_changed, true);
}

ControllerDecoderRecoveryUpdate ControllerDecoderRecoveryCoordinator::update_locked(
    bool state_changed,
    bool request_keyframe) const {
    return {
        .state_changed = state_changed,
        .request_keyframe = request_keyframe,
        .snapshot = snapshot_,
    };
}

namespace {

std::uint64_t default_now_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string_view handoff_state_to_string(HandoffState state) {
    switch (state) {
    case HandoffState::kPreLogin:
        return "pre_login";
    case HandoffState::kLaunchingHelper:
        return "launching_helper";
    case HandoffState::kSynchronizingCapabilities:
        return "synchronizing_capabilities";
    case HandoffState::kUserSessionActive:
        return "user_session_active";
    case HandoffState::kDowngrading:
        return "downgrading";
    case HandoffState::kFailed:
        return "failed";
    }

    return "unknown";
}

std::string_view handoff_event_to_string(HandoffTransitionEvent event) {
    switch (event) {
    case HandoffTransitionEvent::kInteractiveSessionDetected:
        return "interactive_session_detected";
    case HandoffTransitionEvent::kHelperLaunchSucceeded:
        return "helper_launch_succeeded";
    case HandoffTransitionEvent::kHelperLaunchFailed:
        return "helper_launch_failed";
    case HandoffTransitionEvent::kCapabilitySyncSucceeded:
        return "capability_sync_succeeded";
    case HandoffTransitionEvent::kCapabilitySyncFailed:
        return "capability_sync_failed";
    case HandoffTransitionEvent::kUserLoggedOut:
        return "user_logged_out";
    case HandoffTransitionEvent::kHelperLost:
        return "helper_lost";
    case HandoffTransitionEvent::kDowngradeCompleted:
        return "downgrade_completed";
    case HandoffTransitionEvent::kResetToPreLogin:
        return "reset_to_prelogin";
    }

    return "unknown";
}

}  // namespace

IncomingHandshakeProcessor::IncomingHandshakeProcessor(
        security::InMemoryHandshakeReplayGuard& replay_guard,
        security::InMemoryPeerFingerprintVerifier& fingerprint_verifier)
        : replay_guard_(replay_guard),
            fingerprint_verifier_(fingerprint_verifier) {}

IncomingHandshakeResult IncomingHandshakeProcessor::process(std::string_view serialized_handshake) {
    IncomingHandshakeResult result;

    const auto parsed = protocol::parse_handshake_message_v1(serialized_handshake);
    if (!parsed.ok) {
        result.accepted = false;
        result.decision = IncomingHandshakeDecision::rejected_parse;
        result.error = parsed.error;
        return result;
    }

    security::HandshakeReplayCheckRequest replay_request;
    replay_request.session_id = parsed.value.session_id;
    replay_request.nonce = parsed.value.nonce;
    replay_request.created_at_ms = parsed.value.created_at_ms;

    const auto replay_result = replay_guard_.check(replay_request);
    if (!replay_result.accepted) {
        result.accepted = false;
        result.decision = IncomingHandshakeDecision::rejected_replay;
        result.error = replay_result.error;
        result.message = parsed.value;
        return result;
    }

    const auto fingerprint_result = fingerprint_verifier_.verify(parsed.value.responder_fingerprint);
    if (!fingerprint_result.accepted) {
        result.accepted = false;
        result.decision = IncomingHandshakeDecision::rejected_fingerprint;
        result.error = fingerprint_result.error;
        result.message = parsed.value;
        return result;
    }

    result.accepted = true;
    result.decision = IncomingHandshakeDecision::accepted;
    result.message = parsed.value;
    return result;
}

void IncomingHandshakeProcessor::reset_session(std::string_view session_id) {
    replay_guard_.reset_session(session_id);
}

SessionUacConsentIngressRouter::SessionUacConsentIngressRouter(
    service::HostServiceUacConsentSignalProducer& consent_signal_producer,
    std::string expected_session_id)
    : consent_signal_producer_(consent_signal_producer),
      expected_session_id_(std::move(expected_session_id)) {}

IncomingUacConsentResult SessionUacConsentIngressRouter::route(std::string_view serialized_consent) {
    IncomingUacConsentResult result;

    const auto parsed = protocol::parse_uac_consent_envelope_v1(serialized_consent);
    if (!parsed.ok) {
        result.accepted = false;
        result.decision = IncomingUacConsentDecision::rejected_parse;
        result.error = parsed.error;
        return result;
    }

    result.message = parsed.value;
    if (!expected_session_id_.empty() && parsed.value.session_id != expected_session_id_) {
        result.accepted = false;
        result.decision = IncomingUacConsentDecision::rejected_session_mismatch;
        result.error = "uac consent session_id mismatch";
        return result;
    }

    switch (parsed.value.decision) {
    case protocol::UacConsentDecisionV1::allow:
        result.applied_result = consent_signal_producer_.submit_allow(parsed.value.token_id, parsed.value.uac_prompt_id);
        break;
    case protocol::UacConsentDecisionV1::deny:
        result.applied_result = consent_signal_producer_.submit_deny(parsed.value.token_id, parsed.value.uac_prompt_id);
        break;
    case protocol::UacConsentDecisionV1::timeout:
        result.applied_result = consent_signal_producer_.submit_timeout(parsed.value.token_id, parsed.value.uac_prompt_id);
        break;
    }

    result.accepted = true;
    result.decision = IncomingUacConsentDecision::accepted;
    return result;
}

FingerprintTrustPolicyRuntime::FingerprintTrustPolicyRuntime(
    security::InMemoryPeerFingerprintVerifier& verifier,
    security::FileBackedPeerFingerprintStore& store)
    : verifier_(verifier),
      store_(store) {}

bool FingerprintTrustPolicyRuntime::load_on_startup(
    LoadFallbackPolicy fallback_policy,
    std::string* error) {
    return apply_policy(fallback_policy, error);
}

bool FingerprintTrustPolicyRuntime::reload(
    LoadFallbackPolicy fallback_policy,
    std::string* error) {
    return apply_policy(fallback_policy, error);
}

bool FingerprintTrustPolicyRuntime::apply_policy(
    LoadFallbackPolicy fallback_policy,
    std::string* error) {
    std::vector<std::string> fingerprints;
    if (!store_.load(&fingerprints, error)) {
        if (fallback_policy == LoadFallbackPolicy::kClearOnError) {
            verifier_.set_trusted_fingerprints({});
        }
        return false;
    }

    verifier_.set_trusted_fingerprints(fingerprints);
    return true;
}

bool is_valid_session_transition(
    protocol::SessionStateV1 from,
    SessionTransitionEvent event,
    protocol::SessionStateV1 to) {
    switch (from) {
    case protocol::SessionStateV1::idle:
        return event == SessionTransitionEvent::kOfferSent
            && to == protocol::SessionStateV1::offering;

    case protocol::SessionStateV1::offering:
        return event == SessionTransitionEvent::kHandshakeValidated
            && to == protocol::SessionStateV1::connecting;

    case protocol::SessionStateV1::connecting:
        return event == SessionTransitionEvent::kTransportConnected
            && to == protocol::SessionStateV1::established;

    case protocol::SessionStateV1::established:
        return (event == SessionTransitionEvent::kTransportDisconnected
                && to == protocol::SessionStateV1::recovering)
            || (event == SessionTransitionEvent::kTerminateRequested
                && to == protocol::SessionStateV1::terminated);

    case protocol::SessionStateV1::recovering:
        return (event == SessionTransitionEvent::kRecoverySucceeded
                && to == protocol::SessionStateV1::established)
            || (event == SessionTransitionEvent::kRecoveryFailed
                && to == protocol::SessionStateV1::terminated)
            || (event == SessionTransitionEvent::kTerminateRequested
                && to == protocol::SessionStateV1::terminated);

    case protocol::SessionStateV1::terminated:
        return false;
    }

    return false;
}

bool is_valid_handoff_transition(
    HandoffState from,
    HandoffTransitionEvent event,
    HandoffState to) {
    switch (from) {
    case HandoffState::kPreLogin:
        return event == HandoffTransitionEvent::kInteractiveSessionDetected
            && to == HandoffState::kLaunchingHelper;

    case HandoffState::kLaunchingHelper:
        return (event == HandoffTransitionEvent::kHelperLaunchSucceeded
                && to == HandoffState::kSynchronizingCapabilities)
            || (event == HandoffTransitionEvent::kHelperLaunchFailed
                && to == HandoffState::kFailed);

    case HandoffState::kSynchronizingCapabilities:
        return (event == HandoffTransitionEvent::kCapabilitySyncSucceeded
                && to == HandoffState::kUserSessionActive)
            || (event == HandoffTransitionEvent::kCapabilitySyncFailed
                && to == HandoffState::kFailed)
            || (event == HandoffTransitionEvent::kHelperLost
                && to == HandoffState::kDowngrading);

    case HandoffState::kUserSessionActive:
        return (event == HandoffTransitionEvent::kUserLoggedOut
                && to == HandoffState::kDowngrading)
            || (event == HandoffTransitionEvent::kHelperLost
                && to == HandoffState::kDowngrading);

    case HandoffState::kDowngrading:
        return event == HandoffTransitionEvent::kDowngradeCompleted
            && to == HandoffState::kPreLogin;

    case HandoffState::kFailed:
        return event == HandoffTransitionEvent::kResetToPreLogin
            && to == HandoffState::kPreLogin;
    }

    return false;
}

std::string format_handoff_audit_log_line(const HandoffAuditEvent& event) {
    std::string line;
    line.reserve(256);
    line += "ts=" + std::to_string(event.timestamp_unix);
    line += " level=";
    line += event.transitioned ? "info" : "warn";
    line += " component=session.handoff";
    line += " event=";
    line += handoff_event_to_string(event.event);
    line += " session_id=" + event.session_id;
    line += " from=";
    line += handoff_state_to_string(event.from);
    line += " to=";
    line += handoff_state_to_string(event.to);
    line += " transitioned=";
    line += event.transitioned ? "true" : "false";
    if (!event.error.empty()) {
        line += " error=" + event.error;
    }
    return line;
}

SessionHandoffStateMachine::SessionHandoffStateMachine(
    HandoffState initial_state,
    std::string session_id,
    HandoffAuditSink audit_sink,
    NowProvider now_provider)
    : state_(initial_state),
      session_id_(std::move(session_id)),
      audit_sink_(std::move(audit_sink)),
      now_provider_(std::move(now_provider)) {
    if (!now_provider_) {
        now_provider_ = default_now_unix;
    }
}

HandoffState SessionHandoffStateMachine::state() const {
    return state_;
}

HandoffStateUpdate SessionHandoffStateMachine::on_event(HandoffTransitionEvent event) {
    switch (state_) {
    case HandoffState::kPreLogin:
        return apply_transition(
            event,
            HandoffState::kLaunchingHelper,
            "pre-login only accepts interactive session detection");

    case HandoffState::kLaunchingHelper:
        if (event == HandoffTransitionEvent::kHelperLaunchSucceeded) {
            return apply_transition(event, HandoffState::kSynchronizingCapabilities, "launch success transition rejected");
        }
        return apply_transition(event, HandoffState::kFailed, "launching helper transition rejected");

    case HandoffState::kSynchronizingCapabilities:
        if (event == HandoffTransitionEvent::kCapabilitySyncSucceeded) {
            return apply_transition(event, HandoffState::kUserSessionActive, "capability sync success transition rejected");
        }
        if (event == HandoffTransitionEvent::kHelperLost) {
            return apply_transition(event, HandoffState::kDowngrading, "helper-lost transition rejected");
        }
        return apply_transition(event, HandoffState::kFailed, "capability sync transition rejected");

    case HandoffState::kUserSessionActive:
        return apply_transition(event, HandoffState::kDowngrading, "user-session transition rejected");

    case HandoffState::kDowngrading:
        return apply_transition(event, HandoffState::kPreLogin, "downgrade transition rejected");

    case HandoffState::kFailed:
        return apply_transition(event, HandoffState::kPreLogin, "failed transition rejected");
    }

    HandoffStateUpdate update;
    update.state = state_;
    update.error = "unknown handoff state";
    return update;
}

HandoffStateUpdate SessionHandoffStateMachine::apply_transition(
    HandoffTransitionEvent event,
    HandoffState to,
    std::string failure_reason) {
    HandoffStateUpdate update;
    const auto from = state_;
    update.state = from;

    if (!is_valid_handoff_transition(from, event, to)) {
        update.error = std::move(failure_reason);
        emit_audit(from, from, event, false, update.error);
        return update;
    }

    state_ = to;
    update.transitioned = true;
    update.state = state_;
    emit_audit(from, to, event, true, {});
    return update;
}

void SessionHandoffStateMachine::emit_audit(
    HandoffState from,
    HandoffState to,
    HandoffTransitionEvent event,
    bool transitioned,
    std::string_view error) const {
    if (!audit_sink_) {
        return;
    }

    HandoffAuditEvent audit_event;
    audit_event.session_id = session_id_;
    audit_event.from = from;
    audit_event.to = to;
    audit_event.event = event;
    audit_event.transitioned = transitioned;
    audit_event.error = std::string(error);
    audit_event.timestamp_unix = now_provider_();
    audit_sink_(audit_event);
}

std::uint64_t SessionHandoffStateMachine::default_now_unix() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

SessionRecoveryOrchestrator::SessionRecoveryOrchestrator(
    SessionRecoveryPolicy policy,
    protocol::SessionStateV1 initial_state,
    NowProvider now_provider)
    : policy_(policy),
      state_(initial_state),
      now_provider_(std::move(now_provider)) {
    if (!now_provider_) {
        now_provider_ = default_now_ms;
    }
}

const SessionRecoveryPolicy& SessionRecoveryOrchestrator::policy() const {
    return policy_;
}

protocol::SessionStateV1 SessionRecoveryOrchestrator::state() const {
    return state_;
}

std::uint32_t SessionRecoveryOrchestrator::retry_count() const {
    return retry_count_;
}

bool SessionRecoveryOrchestrator::recovery_in_progress() const {
    return state_ == protocol::SessionStateV1::recovering;
}

SessionRecoveryUpdate SessionRecoveryOrchestrator::on_connectivity_signal(ConnectivityHealthSignal signal) {
    if (signal == ConnectivityHealthSignal::kConnected) {
        if (state_ != protocol::SessionStateV1::recovering) {
            SessionRecoveryUpdate update;
            update.state = state_;
            return update;
        }

        const auto update = apply_transition(
            SessionTransitionEvent::kRecoverySucceeded,
            protocol::SessionStateV1::established,
            "recovery success rejected in current state");
        if (update.transitioned) {
            recovery_started_at_ms_ = 0;
            retry_count_ = 0;
            next_retry_allowed_at_ms_ = 0;
        }
        return update;
    }

    if (signal == ConnectivityHealthSignal::kDisconnected
        && state_ == protocol::SessionStateV1::established) {
        const auto update = apply_transition(
            SessionTransitionEvent::kTransportDisconnected,
            protocol::SessionStateV1::recovering,
            "disconnect signal rejected in current state");
        if (update.transitioned) {
            recovery_started_at_ms_ = now_provider_();
            retry_count_ = 0;
            next_retry_allowed_at_ms_ = recovery_started_at_ms_;
        }
        return update;
    }

    if (state_ != protocol::SessionStateV1::recovering) {
        SessionRecoveryUpdate update;
        update.state = state_;
        update.error = "recovery signal ignored outside recovering/established states";
        return update;
    }

    if (signal == ConnectivityHealthSignal::kDisconnected) {
        SessionRecoveryUpdate update;
        update.state = state_;
        return update;
    }

    const std::uint64_t now_ms = now_provider_();
    if (now_ms < next_retry_allowed_at_ms_) {
        SessionRecoveryUpdate update;
        update.state = state_;
        update.error = "retry backoff window active";
        return update;
    }

    ++retry_count_;
    next_retry_allowed_at_ms_ = now_ms + policy_.retry_backoff_ms;
    if (retry_count_ < policy_.max_retry_count) {
        SessionRecoveryUpdate update;
        update.state = state_;
        return update;
    }

    return apply_transition(
        SessionTransitionEvent::kRecoveryFailed,
        protocol::SessionStateV1::terminated,
        "recovery failed transition rejected in current state");
}

SessionRecoveryUpdate SessionRecoveryOrchestrator::on_terminate_requested() {
    const auto update = apply_transition(
        SessionTransitionEvent::kTerminateRequested,
        protocol::SessionStateV1::terminated,
        "terminate transition rejected in current state");
    if (update.transitioned) {
        recovery_started_at_ms_ = 0;
        next_retry_allowed_at_ms_ = 0;
    }
    return update;
}

SessionRecoveryUpdate SessionRecoveryOrchestrator::tick() {
    SessionRecoveryUpdate update;
    update.state = state_;

    if (state_ != protocol::SessionStateV1::recovering) {
        return update;
    }

    const std::uint64_t now_ms = now_provider_();
    if (recovery_started_at_ms_ == 0 || now_ms <= recovery_started_at_ms_) {
        return update;
    }

    const std::uint64_t elapsed = now_ms - recovery_started_at_ms_;
    if (elapsed < policy_.recovery_timeout_ms) {
        return update;
    }

    const auto timeout_update = apply_transition(
        SessionTransitionEvent::kRecoveryFailed,
        protocol::SessionStateV1::terminated,
        "recovery timeout transition rejected in current state");
    if (timeout_update.transitioned) {
        next_retry_allowed_at_ms_ = 0;
    }

    return timeout_update;
}

SessionRecoveryUpdate SessionRecoveryOrchestrator::apply_transition(
    SessionTransitionEvent event,
    protocol::SessionStateV1 to_state,
    std::string failure_reason) {
    SessionRecoveryUpdate update;
    update.state = state_;

    if (!is_valid_session_transition(state_, event, to_state)) {
        update.error = std::move(failure_reason);
        return update;
    }

    state_ = to_state;
    update.transitioned = true;
    update.state = state_;
    return update;
}

PrivilegedInputRuntimeOrchestrator::PrivilegedInputRuntimeOrchestrator(
    input::IInputInjectorBackend& user_desktop_backend,
    input::IInputInjectorBackend& secure_desktop_backend,
    NowProvider now_provider,
    SecureChannelAvailabilitySetter secure_channel_setter)
    : secure_backend_gate_(secure_desktop_backend),
      adapter_(policy_gate_, user_desktop_backend, secure_backend_gate_),
      runtime_controller_(policy_gate_, secure_backend_gate_),
      broker_(
          now_provider,
          {},
          {},
          [this](const service::CapabilityChangeEvent& event) {
              full_control_enabled_ = (event.after == service::CapabilityLevel::kFullControl);
              apply_runtime_state();
          }),
      secure_channel_setter_(std::move(secure_channel_setter)) {
    policy_gate_.set_permission_policy(input::InputPermissionPolicy::kFullControlSecureDesktop);
    apply_runtime_state();
}

service::IPrivilegedControlBroker& PrivilegedInputRuntimeOrchestrator::broker() {
    return broker_;
}

input::TargetRoutingInputInjectionAdapter& PrivilegedInputRuntimeOrchestrator::adapter() {
    return adapter_;
}

void PrivilegedInputRuntimeOrchestrator::on_secure_desktop_channel_ready() {
    set_secure_desktop_channel_available(true);
}

void PrivilegedInputRuntimeOrchestrator::on_secure_desktop_channel_lost() {
    set_secure_desktop_channel_available(false);
}

bool PrivilegedInputRuntimeOrchestrator::on_session_disconnected(std::string_view session_id) {
    const bool revoked = broker_.revokePrivilegedControl(session_id, "transport_disconnected");
    full_control_enabled_ = false;
    secure_channel_available_ = false;
    apply_runtime_state();
    return revoked;
}

void PrivilegedInputRuntimeOrchestrator::set_secure_desktop_channel_available(bool available) {
    secure_channel_available_ = available;
    apply_runtime_state();
}

bool PrivilegedInputRuntimeOrchestrator::secure_desktop_channel_available() const {
    return secure_channel_available_;
}

bool PrivilegedInputRuntimeOrchestrator::full_control_enabled() const {
    return full_control_enabled_;
}

bool PrivilegedInputRuntimeOrchestrator::secure_backend_enabled() const {
    return secure_backend_gate_.enabled();
}

void PrivilegedInputRuntimeOrchestrator::apply_runtime_state() const {
    const bool enable_secure_runtime = full_control_enabled_ && secure_channel_available_;
    runtime_controller_.on_full_control_changed(enable_secure_runtime);

    if (secure_channel_setter_) {
        secure_channel_setter_(secure_channel_available_);
    }
}

HostSessionLifecycleDispatcher::HostSessionLifecycleDispatcher(PrivilegedInputRuntimeOrchestrator& orchestrator)
    : orchestrator_(orchestrator) {}

void HostSessionLifecycleDispatcher::on_host_service_started() const {
    // Start from a fail-closed baseline until the secure channel becomes ready.
    orchestrator_.on_secure_desktop_channel_lost();
}

void HostSessionLifecycleDispatcher::on_secure_desktop_channel_ready() const {
    orchestrator_.on_secure_desktop_channel_ready();
}

void HostSessionLifecycleDispatcher::on_secure_desktop_channel_lost() const {
    orchestrator_.on_secure_desktop_channel_lost();
}

bool HostSessionLifecycleDispatcher::on_transport_disconnected(std::string_view session_id) const {
    return orchestrator_.on_session_disconnected(session_id);
}

bool HostSessionLifecycleDispatcher::on_host_service_stopping(std::string_view session_id) const {
    return orchestrator_.on_session_disconnected(session_id);
}

void bind_host_service_lifecycle_event_source(
    service::IHostServiceLifecycleEventSource& event_source,
    const HostSessionLifecycleDispatcher& dispatcher) {
    event_source.set_events({
        .on_host_service_started = [&dispatcher]() {
            dispatcher.on_host_service_started();
        },
        .on_secure_desktop_channel_ready = [&dispatcher](std::string_view /*session_id*/) {
            dispatcher.on_secure_desktop_channel_ready();
        },
        .on_secure_desktop_channel_lost = [&dispatcher](std::string_view /*session_id*/) {
            dispatcher.on_secure_desktop_channel_lost();
        },
        .on_transport_disconnected = [&dispatcher](std::string_view session_id) {
            (void)dispatcher.on_transport_disconnected(session_id);
        },
        .on_host_service_stopping = [&dispatcher](std::string_view session_id) {
            (void)dispatcher.on_host_service_stopping(session_id);
        },
    });
}

std::string_view module_name() {
    return "session";
}
}
