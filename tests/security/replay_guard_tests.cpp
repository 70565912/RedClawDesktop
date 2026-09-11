#include <cstdint>
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

bool test_accepts_first_handshake() {
    std::uint64_t now_ms = 100000;
    redclaw::security::InMemoryHandshakeReplayGuard guard(
        {},
        [&now_ms]() { return now_ms; });

    redclaw::security::HandshakeReplayCheckRequest request;
    request.session_id = "session-001";
    request.nonce = "nonce-abc123456789";
    request.created_at_ms = 99500;

    const auto result = guard.check(request);
    return expect_true(result.accepted, "first handshake should be accepted")
        && expect_true(result.decision == redclaw::security::HandshakeReplayDecision::accepted, "decision should be accepted");
}

bool test_rejects_duplicate_nonce_in_same_session() {
    std::uint64_t now_ms = 200000;
    redclaw::security::InMemoryHandshakeReplayGuard guard(
        {},
        [&now_ms]() { return now_ms; });

    redclaw::security::HandshakeReplayCheckRequest request;
    request.session_id = "session-002";
    request.nonce = "nonce-dup123456789";
    request.created_at_ms = 199500;

    const auto first = guard.check(request);
    const auto second = guard.check(request);

    return expect_true(first.accepted, "first duplicate test request should be accepted")
        && expect_true(!second.accepted, "duplicate nonce should be rejected")
        && expect_true(second.decision == redclaw::security::HandshakeReplayDecision::rejected_duplicate_nonce, "duplicate decision mismatch");
}

bool test_allows_same_nonce_in_different_sessions() {
    std::uint64_t now_ms = 300000;
    redclaw::security::InMemoryHandshakeReplayGuard guard(
        {},
        [&now_ms]() { return now_ms; });

    redclaw::security::HandshakeReplayCheckRequest first;
    first.session_id = "session-a";
    first.nonce = "nonce-shared123456";
    first.created_at_ms = 299500;

    redclaw::security::HandshakeReplayCheckRequest second;
    second.session_id = "session-b";
    second.nonce = "nonce-shared123456";
    second.created_at_ms = 299500;

    const auto first_result = guard.check(first);
    const auto second_result = guard.check(second);

    return expect_true(first_result.accepted, "first session nonce should be accepted")
        && expect_true(second_result.accepted, "same nonce in different session should be accepted");
}

bool test_rejects_expired_timestamp() {
    std::uint64_t now_ms = 500000;
    redclaw::security::HandshakeReplayGuardConfig config;
    config.max_age_ms = 120000;

    redclaw::security::InMemoryHandshakeReplayGuard guard(
        config,
        [&now_ms]() { return now_ms; });

    redclaw::security::HandshakeReplayCheckRequest request;
    request.session_id = "session-expired";
    request.nonce = "nonce-old123456789";
    request.created_at_ms = 300000;

    const auto result = guard.check(request);
    return expect_true(!result.accepted, "expired handshake should be rejected")
        && expect_true(result.decision == redclaw::security::HandshakeReplayDecision::rejected_expired_timestamp, "expired decision mismatch");
}

bool test_rejects_future_timestamp_beyond_skew() {
    std::uint64_t now_ms = 600000;
    redclaw::security::HandshakeReplayGuardConfig config;
    config.max_future_skew_ms = 1000;

    redclaw::security::InMemoryHandshakeReplayGuard guard(
        config,
        [&now_ms]() { return now_ms; });

    redclaw::security::HandshakeReplayCheckRequest request;
    request.session_id = "session-future";
    request.nonce = "nonce-future123456";
    request.created_at_ms = 602500;

    const auto result = guard.check(request);
    return expect_true(!result.accepted, "future handshake should be rejected")
        && expect_true(result.decision == redclaw::security::HandshakeReplayDecision::rejected_future_timestamp, "future decision mismatch");
}

bool test_session_reset_clears_seen_nonces() {
    std::uint64_t now_ms = 700000;
    redclaw::security::InMemoryHandshakeReplayGuard guard(
        {},
        [&now_ms]() { return now_ms; });

    redclaw::security::HandshakeReplayCheckRequest request;
    request.session_id = "session-reset";
    request.nonce = "nonce-reset1234567";
    request.created_at_ms = 699900;

    const auto first = guard.check(request);
    const auto duplicate = guard.check(request);
    guard.reset_session("session-reset");
    const auto after_reset = guard.check(request);

    return expect_true(first.accepted, "first reset test request should be accepted")
        && expect_true(!duplicate.accepted, "duplicate should be rejected before reset")
        && expect_true(after_reset.accepted, "nonce should be accepted after session reset");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_accepts_first_handshake() && ok;
    ok = test_rejects_duplicate_nonce_in_same_session() && ok;
    ok = test_allows_same_nonce_in_different_sessions() && ok;
    ok = test_rejects_expired_timestamp() && ok;
    ok = test_rejects_future_timestamp_beyond_skew() && ok;
    ok = test_session_reset_clears_seen_nonces() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_security_replay_guard_tests" << '\n';
    return 0;
}
