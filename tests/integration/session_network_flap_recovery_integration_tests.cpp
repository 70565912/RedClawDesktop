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

bool test_network_flap_recovers_within_budget() {
    redclaw::session::SessionRecoveryPolicy policy;
    policy.recovery_timeout_ms = 5000;
    policy.max_retry_count = 3;

    std::uint64_t fake_now = 10000;
    redclaw::session::SessionRecoveryOrchestrator orchestrator(
        policy,
        redclaw::protocol::SessionStateV1::established,
        [&fake_now]() { return fake_now; });

    const std::uint64_t flap_started_at = fake_now;

    const auto disconnected = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kDisconnected);
    if (!expect_true(disconnected.state == redclaw::protocol::SessionStateV1::recovering, "disconnect should enter recovering")) {
        return false;
    }

    fake_now += 1200;
    orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kFailed);

    fake_now += 900;
    const auto recovered = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kConnected);

    const std::uint64_t elapsed = fake_now - flap_started_at;
    return expect_true(recovered.transitioned, "connect signal should transition to established")
        && expect_true(recovered.state == redclaw::protocol::SessionStateV1::established, "state should be established after recovery")
        && expect_true(elapsed < policy.recovery_timeout_ms, "recovery should complete within timeout budget");
}

bool test_flap_exhaustion_terminates_session() {
    redclaw::session::SessionRecoveryPolicy policy;
    policy.recovery_timeout_ms = 5000;
    policy.max_retry_count = 2;
    policy.retry_backoff_ms = 100;

    std::uint64_t fake_now = 20000;
    redclaw::session::SessionRecoveryOrchestrator orchestrator(
        policy,
        redclaw::protocol::SessionStateV1::established,
        [&fake_now]() { return fake_now; });

    orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kDisconnected);
    orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kFailed);
    fake_now += policy.retry_backoff_ms;
    const auto exhausted = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kFailed);

    return expect_true(exhausted.transitioned, "retry exhaustion should transition")
        && expect_true(exhausted.state == redclaw::protocol::SessionStateV1::terminated, "state should be terminated after exhaustion");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_network_flap_recovers_within_budget() && ok;
    ok = test_flap_exhaustion_terminates_session() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_session_network_flap_recovery_integration_tests" << '\n';
    return 0;
}
