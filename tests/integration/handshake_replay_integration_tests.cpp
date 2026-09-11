#include <cstdint>
#include <iostream>
#include <string>

#include "redclaw/protocol/protocol_module.h"
#include "redclaw/security/security_module.h"
#include "redclaw/session/session_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

std::string make_handshake(
    const std::string& session_id,
    const std::string& nonce,
    std::uint64_t created_at_ms,
    const std::string& responder_fingerprint = "sha256:deadbeef") {
    redclaw::protocol::HandshakeMessageV1 message;
    message.session_id = session_id;
    message.nonce = nonce;
    message.created_at_ms = created_at_ms;
    message.initiator_peer_id = "host-alpha";
    message.responder_peer_id = "controller-beta";
    message.responder_fingerprint = responder_fingerprint;
    return redclaw::protocol::serialize_handshake_message_v1(message);
}

bool test_replayed_handshake_is_rejected() {
    std::uint64_t now_ms = 500000;
    redclaw::security::InMemoryHandshakeReplayGuard guard(
        {},
        [&now_ms]() { return now_ms; });
    redclaw::security::InMemoryPeerFingerprintVerifier verifier({"sha256:deadbeef"});
    redclaw::session::IncomingHandshakeProcessor processor(guard, verifier);

    const std::string payload = make_handshake("session-001", "nonce-replay123456", 499500);
    const auto first = processor.process(payload);
    const auto second = processor.process(payload);

    return expect_true(first.accepted, "first handshake should be accepted")
        && expect_true(second.decision == redclaw::session::IncomingHandshakeDecision::rejected_replay, "replayed handshake should be rejected")
        && expect_true(!second.accepted, "second handshake should not be accepted");
}

bool test_invalid_payload_is_rejected_as_parse_error() {
    std::uint64_t now_ms = 700000;
    redclaw::security::InMemoryHandshakeReplayGuard guard(
        {},
        [&now_ms]() { return now_ms; });
    redclaw::security::InMemoryPeerFingerprintVerifier verifier({"sha256:deadbeef"});
    redclaw::session::IncomingHandshakeProcessor processor(guard, verifier);

    const auto result = processor.process("RCD-HANDSHAKE-V1\nsession_id=s1\n");
    return expect_true(!result.accepted, "invalid payload should fail")
        && expect_true(result.decision == redclaw::session::IncomingHandshakeDecision::rejected_parse, "invalid payload should be parse rejection");
}

bool test_reset_session_allows_same_handshake_again() {
    std::uint64_t now_ms = 800000;
    redclaw::security::InMemoryHandshakeReplayGuard guard(
        {},
        [&now_ms]() { return now_ms; });
    redclaw::security::InMemoryPeerFingerprintVerifier verifier({"sha256:deadbeef"});
    redclaw::session::IncomingHandshakeProcessor processor(guard, verifier);

    const std::string payload = make_handshake("session-reset", "nonce-reset123456", 799500);

    const auto first = processor.process(payload);
    const auto duplicate = processor.process(payload);
    processor.reset_session("session-reset");
    const auto after_reset = processor.process(payload);

    return expect_true(first.accepted, "first reset handshake should be accepted")
        && expect_true(!duplicate.accepted, "duplicate before reset should be rejected")
        && expect_true(after_reset.accepted, "same handshake should be accepted after reset");
}

bool test_untrusted_fingerprint_is_rejected() {
    std::uint64_t now_ms = 900000;
    redclaw::security::InMemoryHandshakeReplayGuard guard(
        {},
        [&now_ms]() { return now_ms; });
    redclaw::security::InMemoryPeerFingerprintVerifier verifier({"sha256:deadbeef"});
    redclaw::session::IncomingHandshakeProcessor processor(guard, verifier);

    const std::string payload = make_handshake("session-fp", "nonce-fp123456789", 899500, "sha256:cafebabe");
    const auto result = processor.process(payload);

    return expect_true(!result.accepted, "untrusted fingerprint handshake should be rejected")
        && expect_true(result.decision == redclaw::session::IncomingHandshakeDecision::rejected_fingerprint, "untrusted fingerprint should map to rejected_fingerprint");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_replayed_handshake_is_rejected() && ok;
    ok = test_invalid_payload_is_rejected_as_parse_error() && ok;
    ok = test_reset_session_allows_same_handshake_again() && ok;
    ok = test_untrusted_fingerprint_is_rejected() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_handshake_replay_integration_tests" << '\n';
    return 0;
}
