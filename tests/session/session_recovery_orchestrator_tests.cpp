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

bool test_disconnect_enters_recovery_and_reconnect_restores_established() {
    std::uint64_t fake_now = 1000;

    redclaw::session::SessionRecoveryOrchestrator orchestrator(
        {},
        redclaw::protocol::SessionStateV1::established,
        [&fake_now]() { return fake_now; });

    const auto disconnected = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kDisconnected);
    if (!expect_true(disconnected.transitioned, "disconnect should trigger transition")) {
        return false;
    }
    if (!expect_true(disconnected.state == redclaw::protocol::SessionStateV1::recovering, "state should become recovering")) {
        return false;
    }

    fake_now += 200;
    const auto reconnected = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kConnected);
    return expect_true(reconnected.transitioned, "reconnect should trigger transition")
        && expect_true(reconnected.state == redclaw::protocol::SessionStateV1::established, "state should return to established")
        && expect_true(orchestrator.retry_count() == 0, "retry count should reset after recovery success");
}

bool test_recovery_failure_after_retry_budget_exhausted() {
    redclaw::session::SessionRecoveryPolicy policy;
    policy.max_retry_count = 2;
    policy.retry_backoff_ms = 100;

    std::uint64_t fake_now = 2000;
    redclaw::session::SessionRecoveryOrchestrator orchestrator(
        policy,
        redclaw::protocol::SessionStateV1::established,
        [&fake_now]() { return fake_now; });

    const auto disconnected = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kDisconnected);
    if (!expect_true(disconnected.state == redclaw::protocol::SessionStateV1::recovering, "disconnect should enter recovering")) {
        return false;
    }

    const auto first_fail = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kFailed);
    if (!expect_true(!first_fail.transitioned, "first failure should not terminate yet")) {
        return false;
    }

    const auto immediate_fail = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kFailed);
    if (!expect_true(!immediate_fail.transitioned, "backoff should throttle immediate retry attempts")) {
        return false;
    }
    if (!expect_true(orchestrator.retry_count() == 1, "throttled retry should not increment retry count")) {
        return false;
    }

    fake_now += policy.retry_backoff_ms;
    const auto second_fail = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kFailed);
    return expect_true(second_fail.transitioned, "second failure should terminate by retry budget")
        && expect_true(second_fail.state == redclaw::protocol::SessionStateV1::terminated, "state should become terminated");
}

bool test_recovery_timeout_terminates_session() {
    redclaw::session::SessionRecoveryPolicy policy;
    policy.recovery_timeout_ms = 500;

    std::uint64_t fake_now = 5000;
    redclaw::session::SessionRecoveryOrchestrator orchestrator(
        policy,
        redclaw::protocol::SessionStateV1::established,
        [&fake_now]() { return fake_now; });

    orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kDisconnected);

    fake_now += 400;
    const auto before_timeout = orchestrator.tick();
    if (!expect_true(!before_timeout.transitioned, "tick before timeout should not transition")) {
        return false;
    }

    fake_now += 200;
    const auto timeout_update = orchestrator.tick();
    return expect_true(timeout_update.transitioned, "tick after timeout should transition")
        && expect_true(timeout_update.state == redclaw::protocol::SessionStateV1::terminated, "timeout should terminate session");
}

bool test_invalid_transition_from_idle_is_rejected() {
    redclaw::session::SessionRecoveryOrchestrator orchestrator(
        {},
        redclaw::protocol::SessionStateV1::idle,
        []() { return static_cast<std::uint64_t>(0); });

    const auto disconnected = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kDisconnected);
    return expect_true(!disconnected.transitioned, "idle should reject disconnect recovery transition")
        && expect_true(!disconnected.error.empty(), "invalid transition should return error detail");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_disconnect_enters_recovery_and_reconnect_restores_established() && ok;
    ok = test_recovery_failure_after_retry_budget_exhausted() && ok;
    ok = test_recovery_timeout_terminates_session() && ok;
    ok = test_invalid_transition_from_idle_is_rejected() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_session_recovery_orchestrator_tests" << '\n';
    return 0;
}
