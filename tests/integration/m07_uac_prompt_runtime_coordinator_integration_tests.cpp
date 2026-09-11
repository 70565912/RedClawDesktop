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
    request.session_id = "uac-session";
    request.step_up_proof.operator_id = "uac-operator";
    request.step_up_proof.device_fingerprint = "uac-device";
    request.step_up_proof.challenge_id = std::move(challenge_id);
    request.step_up_proof.signed_proof = "proof";
    request.step_up_proof.issued_at_unix = now;
    return request;
}

bool test_allow_path_through_coordinator() {
    std::uint64_t fake_now = 1'710'003'000ULL;
    redclaw::service::InMemoryPrivilegedControlBroker broker([&fake_now]() { return fake_now; });
    redclaw::service::InMemoryUacPromptLifecycleEventSource lifecycle_source;
    redclaw::service::InMemoryUacConsentActionTransport consent_transport;
    redclaw::service::HostServiceUacPromptRuntimeCoordinator coordinator(broker, lifecycle_source, consent_transport);

    const auto grant = broker.requestPrivilegedControl(make_request(fake_now, "uac-challenge-1"));
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    lifecycle_source.emit_prompt_raised("uac-session", "uac-prompt-1");
    if (!expect_true(coordinator.last_prompt_event_applied(), "prompt raised event should be applied")) {
        return false;
    }

    redclaw::service::UacConsentAction action;
    action.session_id = "uac-session";
    action.token_id = grant.grant_token.token_id;
    action.decision = redclaw::service::PrivilegedDecision::kAllow;
    action.uac_prompt_id = "uac-prompt-1";

    const auto result = consent_transport.submit(action);
    return expect_true(result.applied, "allow decision should apply")
        && expect_true(result.error == redclaw::service::PrivilegedError::kNone, "allow decision should have no error")
        && expect_true(
            broker.currentCapability("uac-session") == redclaw::service::CapabilityLevel::kFullControl,
            "allow decision should keep full-control capability");
}

bool test_unavailable_path_fail_closes_and_blocks_following_consent() {
    std::uint64_t fake_now = 1'710'003'100ULL;
    redclaw::service::InMemoryPrivilegedControlBroker broker([&fake_now]() { return fake_now; });
    redclaw::service::InMemoryUacPromptLifecycleEventSource lifecycle_source;
    redclaw::service::InMemoryUacConsentActionTransport consent_transport;
    redclaw::service::HostServiceUacPromptRuntimeCoordinator coordinator(broker, lifecycle_source, consent_transport);

    const auto grant = broker.requestPrivilegedControl(make_request(fake_now, "uac-challenge-2"));
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    lifecycle_source.emit_prompt_raised("uac-session", "uac-prompt-2");
    if (!expect_true(coordinator.last_prompt_event_applied(), "prompt raised should be applied")) {
        return false;
    }

    lifecycle_source.emit_prompt_unavailable("uac-session", "uac-prompt-2", "secure_desktop_transport_lost");
    if (!expect_true(coordinator.last_prompt_event_applied(), "unavailable event should be applied")) {
        return false;
    }

    if (!expect_true(
            broker.currentCapability("uac-session") == redclaw::service::CapabilityLevel::kStandardControl,
            "unavailable event should downgrade capability")) {
        return false;
    }

    redclaw::service::UacConsentAction action;
    action.session_id = "uac-session";
    action.token_id = grant.grant_token.token_id;
    action.decision = redclaw::service::PrivilegedDecision::kAllow;
    action.uac_prompt_id = "uac-prompt-2";

    const auto result = consent_transport.submit(action);
    return expect_true(!result.applied, "consent should be blocked after unavailable fail-close")
        && expect_true(
            result.error == redclaw::service::PrivilegedError::kPolicyDenied,
            "blocked consent after revoke should report policy denied");
}

bool test_timeout_event_fail_closes() {
    std::uint64_t fake_now = 1'710'003'200ULL;
    redclaw::service::InMemoryPrivilegedControlBroker broker([&fake_now]() { return fake_now; });
    redclaw::service::InMemoryUacPromptLifecycleEventSource lifecycle_source;
    redclaw::service::InMemoryUacConsentActionTransport consent_transport;
    redclaw::service::HostServiceUacPromptRuntimeCoordinator coordinator(broker, lifecycle_source, consent_transport);

    const auto grant = broker.requestPrivilegedControl(make_request(fake_now, "uac-challenge-3"));
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    lifecycle_source.emit_prompt_raised("uac-session", "uac-prompt-3");
    if (!expect_true(coordinator.last_prompt_event_applied(), "prompt raised should be applied")) {
        return false;
    }

    lifecycle_source.emit_prompt_timeout("uac-session", "uac-prompt-3");
    if (!expect_true(coordinator.last_prompt_event_applied(), "timeout event should be applied")) {
        return false;
    }

    return expect_true(
        broker.currentCapability("uac-session") == redclaw::service::CapabilityLevel::kStandardControl,
        "timeout event should downgrade capability to standard");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_allow_path_through_coordinator() && ok;
    ok = test_unavailable_path_fail_closes_and_blocks_following_consent() && ok;
    ok = test_timeout_event_fail_closes() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_m07_uac_prompt_runtime_coordinator_integration_tests" << '\n';
    return 0;
}
