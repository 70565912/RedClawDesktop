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

redclaw::service::PrivilegedRequest make_request(std::uint64_t now, std::string challenge_id) {
    redclaw::service::PrivilegedRequest request;
    request.session_id = "consent-session";
    request.step_up_proof.operator_id = "consent-operator";
    request.step_up_proof.device_fingerprint = "consent-device";
    request.step_up_proof.challenge_id = std::move(challenge_id);
    request.step_up_proof.signed_proof = "proof";
    request.step_up_proof.issued_at_unix = now;
    return request;
}

bool test_allow_signal_keeps_full_control() {
    std::uint64_t fake_now = 1'710'100'000ULL;
    redclaw::service::InMemoryPrivilegedControlBroker broker([&fake_now]() { return fake_now; });
    redclaw::service::InMemoryUacPromptLifecycleEventSource lifecycle_source;
    redclaw::service::InMemoryUacConsentActionTransport consent_transport;
    redclaw::service::HostServiceUacPromptRuntimeCoordinator coordinator(broker, lifecycle_source, consent_transport);
    redclaw::service::HostServiceUacConsentSignalProducer consent_producer(consent_transport, "consent-session");

    const auto grant = broker.requestPrivilegedControl(make_request(fake_now, "consent-challenge-1"));
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    lifecycle_source.emit_prompt_raised("consent-session", "prompt-allow");
    if (!expect_true(coordinator.last_prompt_event_applied(), "prompt raised should be applied")) {
        return false;
    }

    const auto result = consent_producer.submit_allow(grant.grant_token.token_id, "prompt-allow");
    return expect_true(result.applied, "allow consent should be applied")
        && expect_true(result.error == redclaw::service::PrivilegedError::kNone, "allow consent should return no error")
        && expect_true(
            broker.currentCapability("consent-session") == redclaw::service::CapabilityLevel::kFullControl,
            "allow consent should keep full-control capability");
}

bool test_deny_signal_applies_decision() {
    std::uint64_t fake_now = 1'710'100'100ULL;
    redclaw::service::InMemoryPrivilegedControlBroker broker([&fake_now]() { return fake_now; });
    redclaw::service::InMemoryUacPromptLifecycleEventSource lifecycle_source;
    redclaw::service::InMemoryUacConsentActionTransport consent_transport;
    redclaw::service::HostServiceUacPromptRuntimeCoordinator coordinator(broker, lifecycle_source, consent_transport);
    redclaw::service::HostServiceUacConsentSignalProducer consent_producer(consent_transport, "consent-session");

    const auto grant = broker.requestPrivilegedControl(make_request(fake_now, "consent-challenge-2"));
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    lifecycle_source.emit_prompt_raised("consent-session", "prompt-deny");
    if (!expect_true(coordinator.last_prompt_event_applied(), "prompt raised should be applied")) {
        return false;
    }

    const auto result = consent_producer.submit_deny(grant.grant_token.token_id, "prompt-deny");
    return expect_true(result.applied, "deny consent should be applied")
        && expect_true(result.final_decision == redclaw::service::PrivilegedDecision::kDeny, "deny consent should return deny decision")
        && expect_true(
            broker.currentCapability("consent-session") == redclaw::service::CapabilityLevel::kFullControl,
            "deny decision should not auto-revoke in broker confirm path");
}

bool test_timeout_signal_fail_closes_after_prompt_timeout() {
    std::uint64_t fake_now = 1'710'100'200ULL;
    redclaw::service::InMemoryPrivilegedControlBroker broker([&fake_now]() { return fake_now; });
    redclaw::service::InMemoryUacPromptLifecycleEventSource lifecycle_source;
    redclaw::service::InMemoryUacConsentActionTransport consent_transport;
    redclaw::service::HostServiceUacPromptRuntimeCoordinator coordinator(broker, lifecycle_source, consent_transport);
    redclaw::service::HostServiceUacConsentSignalProducer consent_producer(consent_transport, "consent-session");

    const auto grant = broker.requestPrivilegedControl(make_request(fake_now, "consent-challenge-3"));
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    lifecycle_source.emit_prompt_raised("consent-session", "prompt-timeout");
    if (!expect_true(coordinator.last_prompt_event_applied(), "prompt raised should be applied")) {
        return false;
    }

    lifecycle_source.emit_prompt_timeout("consent-session", "prompt-timeout");
    if (!expect_true(coordinator.last_prompt_event_applied(), "prompt timeout should be applied")) {
        return false;
    }

    const auto result = consent_producer.submit_timeout(grant.grant_token.token_id, "prompt-timeout");
    return expect_true(!result.applied, "timeout signal should be blocked after fail-close")
        && expect_true(result.error == redclaw::service::PrivilegedError::kPolicyDenied, "blocked timeout signal should be policy denied")
        && expect_true(
            broker.currentCapability("consent-session") == redclaw::service::CapabilityLevel::kStandardControl,
            "timeout lifecycle event should revoke capability");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_allow_signal_keeps_full_control() && ok;
    ok = test_deny_signal_applies_decision() && ok;
    ok = test_timeout_signal_fail_closes_after_prompt_timeout() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_m07_uac_consent_signal_producer_integration_tests" << '\n';
    return 0;
}
