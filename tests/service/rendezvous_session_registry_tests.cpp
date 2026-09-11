#include <cstdint>
#include <iostream>
#include <string>

#include "redclaw/service/service_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_register_lookup_and_claim_flow() {
    std::uint64_t now = 1000;
    redclaw::service::InMemoryRendezvousSessionRegistry registry([&now]() {
        return now;
    });

    redclaw::service::RendezvousSessionRegistration registration;
    registration.session_code = "A1B2C3D4";
    registration.session_id = "session-host-001";
    registration.host_display_name = "Host Alpha";
    registration.host_fingerprint_summary = "fp:ab12cd34";
    registration.ttl_seconds = 300;

    const auto register_result = registry.register_session(registration);
    bool ok = true;
    ok = expect_true(register_result.accepted, "register_session should accept valid registration") && ok;
    ok = expect_true(
        register_result.error == redclaw::service::RendezvousRegistryError::kNone,
        "register_session should return kNone for valid registration") && ok;
    ok = expect_true(register_result.expires_at_unix == 1300, "register_session should return expected expiry") && ok;

    const auto lookup_result = registry.lookup_session("A1B2C3D4");
    ok = expect_true(lookup_result.found, "lookup_session should find registered code") && ok;
    ok = expect_true(!lookup_result.claimed, "lookup_session should report unclaimed before claim") && ok;
    ok = expect_true(lookup_result.session_id == "session-host-001", "lookup_session should return session id") && ok;
    ok = expect_true(lookup_result.host_display_name == "Host Alpha", "lookup_session should return host display") && ok;

    const auto claim_result = registry.claim_session("A1B2C3D4");
    ok = expect_true(claim_result.claimed, "claim_session should succeed for unclaimed code") && ok;
    ok = expect_true(
        claim_result.error == redclaw::service::RendezvousRegistryError::kNone,
        "claim_session should return kNone on first claim") && ok;

    const auto second_claim = registry.claim_session("A1B2C3D4");
    ok = expect_true(!second_claim.claimed, "claim_session should fail on second claim") && ok;
    ok = expect_true(
        second_claim.error == redclaw::service::RendezvousRegistryError::kCodeAlreadyClaimed,
        "second claim should report already claimed") && ok;

    return ok;
}

bool test_register_rejects_duplicate_code() {
    std::uint64_t now = 2000;
    redclaw::service::InMemoryRendezvousSessionRegistry registry([&now]() {
        return now;
    });

    redclaw::service::RendezvousSessionRegistration first;
    first.session_code = "Q1W2E3R4";
    first.session_id = "session-1";
    first.host_display_name = "Host One";
    first.host_fingerprint_summary = "fp:1111";
    first.ttl_seconds = 120;

    redclaw::service::RendezvousSessionRegistration second = first;
    second.session_id = "session-2";

    const auto first_result = registry.register_session(first);
    const auto second_result = registry.register_session(second);

    return expect_true(first_result.accepted, "first registration should succeed")
        && expect_true(!second_result.accepted, "second registration should fail for duplicate code")
        && expect_true(
            second_result.error == redclaw::service::RendezvousRegistryError::kCodeAlreadyExists,
            "duplicate registration should report code exists");
}

bool test_expired_code_is_removed() {
    std::uint64_t now = 3000;
    redclaw::service::InMemoryRendezvousSessionRegistry registry([&now]() {
        return now;
    });

    redclaw::service::RendezvousSessionRegistration registration;
    registration.session_code = "Z9Y8X7W6";
    registration.session_id = "session-expire";
    registration.host_display_name = "Host Expire";
    registration.host_fingerprint_summary = "fp:2222";
    registration.ttl_seconds = 10;

    const auto register_result = registry.register_session(registration);
    if (!expect_true(register_result.accepted, "registration should succeed before expiry test")) {
        return false;
    }

    now = 3015;
    const auto lookup_after_expiry = registry.lookup_session("Z9Y8X7W6");
    bool ok = true;
    ok = expect_true(!lookup_after_expiry.found, "expired code should not be found") && ok;
    ok = expect_true(
        lookup_after_expiry.error == redclaw::service::RendezvousRegistryError::kCodeExpired,
        "expired lookup should report code expired") && ok;

    const auto lookup_after_purge = registry.lookup_session("Z9Y8X7W6");
    ok = expect_true(
        lookup_after_purge.error == redclaw::service::RendezvousRegistryError::kCodeNotFound,
        "second lookup should report code not found after purge") && ok;

    return ok;
}

bool test_invalid_requests_rejected() {
    std::uint64_t now = 4000;
    redclaw::service::InMemoryRendezvousSessionRegistry registry([&now]() {
        return now;
    });

    redclaw::service::RendezvousSessionRegistration registration;
    registration.session_code = "bad-code";
    registration.session_id = "session-invalid";
    registration.host_display_name = "Host Invalid";
    registration.host_fingerprint_summary = "fp:3333";
    registration.ttl_seconds = 300;

    const auto register_result = registry.register_session(registration);
    const auto lookup_result = registry.lookup_session("1234");
    const auto claim_result = registry.claim_session("####");

    return expect_true(
               !register_result.accepted && register_result.error == redclaw::service::RendezvousRegistryError::kInvalidRequest,
               "invalid registration should be rejected")
        && expect_true(
               lookup_result.error == redclaw::service::RendezvousRegistryError::kInvalidRequest,
               "invalid lookup code should be rejected")
        && expect_true(
               claim_result.error == redclaw::service::RendezvousRegistryError::kInvalidRequest,
               "invalid claim code should be rejected");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_register_lookup_and_claim_flow() && ok;
    ok = test_register_rejects_duplicate_code() && ok;
    ok = test_expired_code_is_removed() && ok;
    ok = test_invalid_requests_rejected() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_service_rendezvous_session_registry_tests" << '\n';
    return 0;
}
