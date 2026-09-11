#include <iostream>
#include <string>

#include "redclaw/security/security_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_accepts_trusted_fingerprint() {
    redclaw::security::InMemoryPeerFingerprintVerifier verifier({"sha256:deadbeef"});
    const auto result = verifier.verify("sha256:deadbeef");

    return expect_true(result.accepted, "trusted fingerprint should be accepted")
        && expect_true(result.decision == redclaw::security::PeerFingerprintDecision::accepted, "decision should be accepted");
}

bool test_rejects_untrusted_fingerprint() {
    redclaw::security::InMemoryPeerFingerprintVerifier verifier({"sha256:deadbeef"});
    const auto result = verifier.verify("sha256:cafebabe");

    return expect_true(!result.accepted, "untrusted fingerprint should be rejected")
        && expect_true(result.decision == redclaw::security::PeerFingerprintDecision::rejected_untrusted, "decision should be rejected_untrusted");
}

bool test_rejects_invalid_fingerprint_format() {
    redclaw::security::InMemoryPeerFingerprintVerifier verifier({"sha256:deadbeef"});
    const auto result = verifier.verify("fingerprint-deadbeef");

    return expect_true(!result.accepted, "invalid-format fingerprint should be rejected")
        && expect_true(result.decision == redclaw::security::PeerFingerprintDecision::rejected_invalid_input, "decision should be rejected_invalid_input");
}

bool test_trust_and_revoke_flow() {
    redclaw::security::InMemoryPeerFingerprintVerifier verifier;
    verifier.trust_fingerprint("sha256:bead1234");

    const auto trusted = verifier.verify("sha256:bead1234");
    verifier.revoke_fingerprint("sha256:bead1234");
    const auto revoked = verifier.verify("sha256:bead1234");

    return expect_true(trusted.accepted, "trusted fingerprint should pass")
        && expect_true(!revoked.accepted, "revoked fingerprint should fail")
        && expect_true(revoked.decision == redclaw::security::PeerFingerprintDecision::rejected_untrusted, "revoked decision should be rejected_untrusted");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_accepts_trusted_fingerprint() && ok;
    ok = test_rejects_untrusted_fingerprint() && ok;
    ok = test_rejects_invalid_fingerprint_format() && ok;
    ok = test_trust_and_revoke_flow() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_security_peer_fingerprint_verifier_tests" << '\n';
    return 0;
}
