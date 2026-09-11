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

bool test_peer_loopback_poc() {
    const auto result = redclaw::net::run_peer_loopback_poc();
    return expect_true(result.ok, std::string("loopback PoC should pass: ") + result.error)
        && expect_true(result.received_message == "redclaw-m01-loopback", "loopback message mismatch");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_peer_loopback_poc() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_net_peer_loopback_poc_tests" << '\n';
    return 0;
}
