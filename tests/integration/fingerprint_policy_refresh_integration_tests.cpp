#include <cstdint>
#include <filesystem>
#include <fstream>
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

std::string make_temp_file_path(const std::string& suffix) {
    const auto base = std::filesystem::temp_directory_path();
    const auto tick = std::to_string(static_cast<unsigned long long>(std::filesystem::file_time_type::clock::now().time_since_epoch().count()));
    return (base / ("redclaw_fp_policy_" + tick + suffix)).string();
}

std::string make_handshake(
    const std::string& session_id,
    const std::string& nonce,
    std::uint64_t created_at_ms,
    const std::string& responder_fingerprint) {
    redclaw::protocol::HandshakeMessageV1 message;
    message.session_id = session_id;
    message.nonce = nonce;
    message.created_at_ms = created_at_ms;
    message.initiator_peer_id = "host-alpha";
    message.responder_peer_id = "controller-beta";
    message.responder_fingerprint = responder_fingerprint;
    return redclaw::protocol::serialize_handshake_message_v1(message);
}

bool test_runtime_reload_applies_new_trust_policy() {
    const std::string path = make_temp_file_path("_trust.txt");
    redclaw::security::FileBackedPeerFingerprintStore store(path);
    std::string error;

    if (!expect_true(store.save({"sha256:deadbeef"}, &error), "initial trust-store save should succeed")) {
        std::filesystem::remove(path);
        return false;
    }

    redclaw::security::InMemoryPeerFingerprintVerifier verifier;
    redclaw::session::FingerprintTrustPolicyRuntime runtime(verifier, store);
    if (!expect_true(
            runtime.load_on_startup(
                redclaw::session::FingerprintTrustPolicyRuntime::LoadFallbackPolicy::kClearOnError,
                &error),
            "startup trust policy load should succeed")) {
        std::filesystem::remove(path);
        return false;
    }

    std::uint64_t now_ms = 1'000'000;
    redclaw::security::InMemoryHandshakeReplayGuard replay_guard(
        {},
        [&now_ms]() { return now_ms; });
    redclaw::session::IncomingHandshakeProcessor processor(replay_guard, verifier);

    const auto trusted_before = processor.process(make_handshake("session-before-1", "nonce-before111111", 999500, "sha256:deadbeef"));
    const auto untrusted_before = processor.process(make_handshake("session-before-2", "nonce-before222222", 999500, "sha256:cafebabe"));

    if (!expect_true(trusted_before.accepted, "initial trusted fingerprint should be accepted")
        || !expect_true(!untrusted_before.accepted, "initial untrusted fingerprint should be rejected")
        || !expect_true(untrusted_before.decision == redclaw::session::IncomingHandshakeDecision::rejected_fingerprint, "initial untrusted decision should be rejected_fingerprint")) {
        std::filesystem::remove(path);
        return false;
    }

    if (!expect_true(store.save({"sha256:cafebabe"}, &error), "updated trust-store save should succeed")) {
        std::filesystem::remove(path);
        return false;
    }
    if (!expect_true(
            runtime.reload(
                redclaw::session::FingerprintTrustPolicyRuntime::LoadFallbackPolicy::kRetainExistingOnError,
                &error),
            "runtime reload should succeed")) {
        std::filesystem::remove(path);
        return false;
    }

    const auto old_after_reload = processor.process(make_handshake("session-after-1", "nonce-after1111111", 999500, "sha256:deadbeef"));
    const auto new_after_reload = processor.process(make_handshake("session-after-2", "nonce-after2222222", 999500, "sha256:cafebabe"));

    std::filesystem::remove(path);
    return expect_true(!old_after_reload.accepted, "old fingerprint should be rejected after reload")
        && expect_true(old_after_reload.decision == redclaw::session::IncomingHandshakeDecision::rejected_fingerprint, "old fingerprint should fail by fingerprint decision")
        && expect_true(new_after_reload.accepted, "new fingerprint should be accepted after reload");
}

bool test_startup_missing_store_clear_fallback_clears_trust() {
    const std::string path = make_temp_file_path("_missing.txt");
    std::filesystem::remove(path);

    redclaw::security::FileBackedPeerFingerprintStore store(path);
    redclaw::security::InMemoryPeerFingerprintVerifier verifier({"sha256:deadbeef"});
    redclaw::session::FingerprintTrustPolicyRuntime runtime(verifier, store);

    std::string error;
    const bool ok = runtime.load_on_startup(
        redclaw::session::FingerprintTrustPolicyRuntime::LoadFallbackPolicy::kClearOnError,
        &error);

    const auto result = verifier.verify("sha256:deadbeef");
    return expect_true(!ok, "missing trust store should fail startup load")
        && expect_true(!result.accepted, "clear fallback should clear existing trust entries");
}

bool test_startup_missing_store_retain_fallback_keeps_trust() {
    const std::string path = make_temp_file_path("_missing_retain.txt");
    std::filesystem::remove(path);

    redclaw::security::FileBackedPeerFingerprintStore store(path);
    redclaw::security::InMemoryPeerFingerprintVerifier verifier({"sha256:deadbeef"});
    redclaw::session::FingerprintTrustPolicyRuntime runtime(verifier, store);

    std::string error;
    const bool ok = runtime.load_on_startup(
        redclaw::session::FingerprintTrustPolicyRuntime::LoadFallbackPolicy::kRetainExistingOnError,
        &error);

    const auto result = verifier.verify("sha256:deadbeef");
    return expect_true(!ok, "missing trust store should fail startup load")
        && expect_true(result.accepted, "retain fallback should keep existing trust entries");
}

bool test_startup_invalid_store_clear_fallback_clears_trust() {
    const std::string path = make_temp_file_path("_invalid.txt");
    {
        std::ofstream out(path, std::ios::trunc);
        out << "sha256:deadbeef\n";
        out << "not-a-fingerprint\n";
    }

    redclaw::security::FileBackedPeerFingerprintStore store(path);
    redclaw::security::InMemoryPeerFingerprintVerifier verifier({"sha256:deadbeef"});
    redclaw::session::FingerprintTrustPolicyRuntime runtime(verifier, store);

    std::string error;
    const bool ok = runtime.load_on_startup(
        redclaw::session::FingerprintTrustPolicyRuntime::LoadFallbackPolicy::kClearOnError,
        &error);

    std::filesystem::remove(path);
    const auto result = verifier.verify("sha256:deadbeef");
    return expect_true(!ok, "invalid trust store should fail startup load")
        && expect_true(!result.accepted, "clear fallback should clear trust entries on invalid store");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_runtime_reload_applies_new_trust_policy() && ok;
    ok = test_startup_missing_store_clear_fallback_clears_trust() && ok;
    ok = test_startup_missing_store_retain_fallback_keeps_trust() && ok;
    ok = test_startup_invalid_store_clear_fallback_clears_trust() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_fingerprint_policy_refresh_integration_tests" << '\n';
    return 0;
}
