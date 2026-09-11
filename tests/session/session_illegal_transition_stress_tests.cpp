#include <array>
#include <cstdint>
#include <iostream>
#include <string>

#include "redclaw/protocol/protocol_module.h"
#include "redclaw/session/session_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool is_legal_transition(
    redclaw::protocol::SessionStateV1 from,
    redclaw::session::SessionTransitionEvent event,
    redclaw::protocol::SessionStateV1 to) {
    using redclaw::protocol::SessionStateV1;
    using redclaw::session::SessionTransitionEvent;

    if (from == SessionStateV1::idle) {
        return event == SessionTransitionEvent::kOfferSent && to == SessionStateV1::offering;
    }
    if (from == SessionStateV1::offering) {
        return event == SessionTransitionEvent::kHandshakeValidated && to == SessionStateV1::connecting;
    }
    if (from == SessionStateV1::connecting) {
        return event == SessionTransitionEvent::kTransportConnected && to == SessionStateV1::established;
    }
    if (from == SessionStateV1::established) {
        return (event == SessionTransitionEvent::kTransportDisconnected && to == SessionStateV1::recovering)
            || (event == SessionTransitionEvent::kTerminateRequested && to == SessionStateV1::terminated);
    }
    if (from == SessionStateV1::recovering) {
        return (event == SessionTransitionEvent::kRecoverySucceeded && to == SessionStateV1::established)
            || (event == SessionTransitionEvent::kRecoveryFailed && to == SessionStateV1::terminated)
            || (event == SessionTransitionEvent::kTerminateRequested && to == SessionStateV1::terminated);
    }

    return false;
}

bool test_cross_product_invalid_transitions_are_rejected() {
    using redclaw::protocol::SessionStateV1;
    using redclaw::session::SessionTransitionEvent;
    using redclaw::session::is_valid_session_transition;

    constexpr std::array<SessionStateV1, 6> kStates = {
        SessionStateV1::idle,
        SessionStateV1::offering,
        SessionStateV1::connecting,
        SessionStateV1::established,
        SessionStateV1::recovering,
        SessionStateV1::terminated,
    };

    constexpr std::array<SessionTransitionEvent, 7> kEvents = {
        SessionTransitionEvent::kOfferSent,
        SessionTransitionEvent::kHandshakeValidated,
        SessionTransitionEvent::kTransportConnected,
        SessionTransitionEvent::kTransportDisconnected,
        SessionTransitionEvent::kRecoverySucceeded,
        SessionTransitionEvent::kRecoveryFailed,
        SessionTransitionEvent::kTerminateRequested,
    };

    bool ok = true;
    for (const auto from : kStates) {
        for (const auto event : kEvents) {
            for (const auto to : kStates) {
                const bool expected = is_legal_transition(from, event, to);
                const bool actual = is_valid_session_transition(from, event, to);
                if (!expected) {
                    ok = expect_true(!actual, "invalid transition should be rejected") && ok;
                }
            }
        }
    }

    return ok;
}

bool test_repeated_terminate_requests_preserve_terminal_state() {
    using redclaw::protocol::SessionStateV1;

    redclaw::session::SessionRecoveryOrchestrator orchestrator(
        {},
        SessionStateV1::established,
        []() { return static_cast<std::uint64_t>(1000); });

    const auto first = orchestrator.on_terminate_requested();
    if (!expect_true(first.transitioned, "first terminate should transition")) {
        return false;
    }
    if (!expect_true(first.state == SessionStateV1::terminated, "first terminate should reach terminated")) {
        return false;
    }

    const auto second = orchestrator.on_terminate_requested();
    return expect_true(!second.transitioned, "second terminate should be rejected in terminal state")
        && expect_true(second.state == SessionStateV1::terminated, "terminal state must remain unchanged")
        && expect_true(!second.error.empty(), "rejected terminate should return error detail");
}

bool test_out_of_order_connected_signal_rejected_outside_recovering() {
    using redclaw::protocol::SessionStateV1;

    redclaw::session::SessionRecoveryOrchestrator orchestrator(
        {},
        SessionStateV1::established,
        []() { return static_cast<std::uint64_t>(1000); });

    const auto update = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kConnected);
    return expect_true(!update.transitioned, "connected signal outside recovering should be ignored")
        && expect_true(update.state == SessionStateV1::established, "state must remain unchanged after ignored signal")
        && expect_true(update.error.empty(), "ignored signal should not emit error detail");
}

bool test_burst_event_sequence_does_not_corrupt_state() {
    using redclaw::protocol::SessionStateV1;

    std::uint64_t fake_now = 5000;
    redclaw::session::SessionRecoveryOrchestrator orchestrator(
        {},
        SessionStateV1::established,
        [&fake_now]() { return fake_now; });

    const auto to_recovering = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kDisconnected);
    if (!expect_true(to_recovering.transitioned, "disconnect should enter recovering")) {
        return false;
    }

    const auto fail_1 = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kFailed);
    if (!expect_true(!fail_1.transitioned, "first fail during recovering should not terminate immediately")) {
        return false;
    }

    const auto recovered = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kConnected);
    if (!expect_true(recovered.transitioned, "connected should recover to established")) {
        return false;
    }

    const auto invalid_recovery_fail = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kConnected);
    if (!expect_true(!invalid_recovery_fail.transitioned, "second connected in established should be ignored")) {
        return false;
    }

    return expect_true(orchestrator.state() == SessionStateV1::established, "burst sequence should end in stable established state");
}

bool test_transport_signal_ignored_after_terminated() {
    using redclaw::protocol::SessionStateV1;

    redclaw::session::SessionRecoveryOrchestrator orchestrator(
        {},
        SessionStateV1::recovering,
        []() { return static_cast<std::uint64_t>(2000); });

    const auto terminated = orchestrator.on_terminate_requested();
    if (!expect_true(terminated.transitioned, "terminate from recovering should transition")) {
        return false;
    }

    const auto after = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kFailed);
    return expect_true(!after.transitioned, "signals after terminated should not transition")
        && expect_true(orchestrator.state() == SessionStateV1::terminated, "state must stay terminated");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_cross_product_invalid_transitions_are_rejected() && ok;
    ok = test_repeated_terminate_requests_preserve_terminal_state() && ok;
    ok = test_out_of_order_connected_signal_rejected_outside_recovering() && ok;
    ok = test_burst_event_sequence_does_not_corrupt_state() && ok;
    ok = test_transport_signal_ignored_after_terminated() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_session_illegal_transition_stress_tests" << '\n';
    return 0;
}
