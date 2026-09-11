#include <iostream>
#include <string>

#include "support/peer_harness.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_direct_lan_connect_success() {
    redclaw::net::LanPeerIntegrationHarnessConfig config;
    config.timeout_ms = 8000;
    config.block_direct_candidates = false;
    config.enable_relay_fallback = true;

    const auto result = redclaw::net::run_lan_peer_integration_harness(config);
    return expect_true(result.ok, std::string("direct lan harness should succeed: ") + result.error)
        && expect_true(!result.used_relay_fallback, "direct lan harness should not mark relay fallback")
        && expect_true(result.received_message == "redclaw-m01-loopback", "direct lan harness message mismatch");
}

bool test_relay_fallback_after_direct_block() {
    redclaw::net::LanPeerIntegrationHarnessConfig config;
    config.timeout_ms = 8000;
    config.block_direct_candidates = true;
    config.enable_relay_fallback = true;

    const auto result = redclaw::net::run_lan_peer_integration_harness(config);
    return expect_true(result.ok, std::string("relay fallback harness should succeed: ") + result.error)
        && expect_true(result.used_relay_fallback, "relay fallback should be marked as used")
        && expect_true(result.received_message == "redclaw-m01-loopback", "relay fallback message mismatch");
}

bool test_direct_block_without_fallback_fails() {
    redclaw::net::LanPeerIntegrationHarnessConfig config;
    config.timeout_ms = 2500;
    config.block_direct_candidates = true;
    config.enable_relay_fallback = false;

    const auto result = redclaw::net::run_lan_peer_integration_harness(config);
    return expect_true(!result.ok, "harness should fail when direct path is blocked and fallback disabled")
        && expect_true(!result.used_relay_fallback, "fallback-disabled run should not mark relay as used")
        && expect_true(!result.error.empty(), "failure should include an error message");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_direct_lan_connect_success() && ok;
    ok = test_relay_fallback_after_direct_block() && ok;
    ok = test_direct_block_without_fallback_fails() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_net_lan_peer_integration_tests" << '\n';
    return 0;
}
