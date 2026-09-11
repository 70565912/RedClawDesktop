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

bool test_repeated_flap_cycles_recover_without_termination() {
    redclaw::session::SessionRecoveryPolicy policy;
    policy.recovery_timeout_ms = 8000;
    policy.max_retry_count = 4;
    policy.retry_backoff_ms = 80;

    std::uint64_t fake_now = 1000;
    redclaw::session::SessionRecoveryOrchestrator orchestrator(
        policy,
        redclaw::protocol::SessionStateV1::established,
        [&fake_now]() { return fake_now; });

    for (std::uint32_t i = 0; i < 240; ++i) {
        const auto disconnected = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kDisconnected);
        if (!expect_true(disconnected.transitioned, "disconnect should enter recovering")) {
            return false;
        }

        const std::uint32_t fail_signals = i % 3;
        for (std::uint32_t j = 0; j < fail_signals; ++j) {
            fake_now += policy.retry_backoff_ms;
            const auto failed = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kFailed);
            if (!expect_true(!failed.transitioned, "budgeted failure signal should not terminate")) {
                return false;
            }
        }

        fake_now += policy.retry_backoff_ms;
        const auto recovered = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kConnected);
        if (!expect_true(recovered.transitioned, "connect should recover session")) {
            return false;
        }
        if (!expect_true(recovered.state == redclaw::protocol::SessionStateV1::established, "recovered state should be established")) {
            return false;
        }
        if (!expect_true(orchestrator.retry_count() == 0, "retry counter should reset after successful reconnect")) {
            return false;
        }

        // Noise while already established should be ignored and never force a state transition.
        const auto noise_connected = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kConnected);
        if (!expect_true(!noise_connected.transitioned, "spurious connected should be ignored in established")) {
            return false;
        }
        if (!expect_true(noise_connected.state == redclaw::protocol::SessionStateV1::established, "spurious connected should keep established")) {
            return false;
        }
    }

    return expect_true(orchestrator.state() == redclaw::protocol::SessionStateV1::established, "chaos flap loop should end in established");
}

bool test_retry_backoff_throttles_burst_failure_signals() {
    redclaw::session::SessionRecoveryPolicy policy;
    policy.recovery_timeout_ms = 5000;
    policy.max_retry_count = 2;
    policy.retry_backoff_ms = 120;

    std::uint64_t fake_now = 3000;
    redclaw::session::SessionRecoveryOrchestrator orchestrator(
        policy,
        redclaw::protocol::SessionStateV1::established,
        [&fake_now]() { return fake_now; });

    orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kDisconnected);

    const auto first_fail = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kFailed);
    if (!expect_true(!first_fail.transitioned, "first fail should stay in recovering")) {
        return false;
    }

    const auto throttled = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kFailed);
    if (!expect_true(!throttled.transitioned, "burst fail should be throttled by backoff")) {
        return false;
    }
    if (!expect_true(orchestrator.retry_count() == 1, "throttled signal should not consume retry budget")) {
        return false;
    }

    fake_now += policy.retry_backoff_ms;
    const auto exhausted = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kFailed);
    return expect_true(exhausted.transitioned, "post-backoff fail should consume retry budget")
        && expect_true(exhausted.state == redclaw::protocol::SessionStateV1::terminated, "retry budget exhaustion should terminate");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_repeated_flap_cycles_recover_without_termination() && ok;
    ok = test_retry_backoff_throttles_burst_failure_signals() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_session_network_flap_chaos_hardening_tests" << '\n';
    return 0;
}
