#include <cstdint>
#include <iostream>
#include <string>

#include "redclaw/protocol/protocol_module.h"
#include "redclaw/service/service_module.h"
#include "redclaw/session/session_module.h"

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
    request.session_id = "router-session";
    request.step_up_proof.operator_id = "router-operator";
    request.step_up_proof.device_fingerprint = "router-device";
    request.step_up_proof.challenge_id = std::move(challenge_id);
    request.step_up_proof.signed_proof = "proof";
    request.step_up_proof.issued_at_unix = now;
    return request;
}

bool test_router_applies_allow_message() {
    std::uint64_t fake_now = 1'710'200'000ULL;
    redclaw::service::InMemoryPrivilegedControlBroker broker([&fake_now]() { return fake_now; });
    redclaw::service::InMemoryUacPromptLifecycleEventSource lifecycle_source;
    redclaw::service::InMemoryUacConsentActionTransport consent_transport;
    redclaw::service::HostServiceUacPromptRuntimeCoordinator coordinator(broker, lifecycle_source, consent_transport);
    redclaw::service::HostServiceUacConsentSignalProducer consent_producer(consent_transport, "router-session");
    redclaw::session::SessionUacConsentIngressRouter router(consent_producer, "router-session");

    const auto grant = broker.requestPrivilegedControl(make_request(fake_now, "router-challenge-1"));
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    lifecycle_source.emit_prompt_raised("router-session", "prompt-allow");
    if (!expect_true(coordinator.last_prompt_event_applied(), "prompt raised should be applied")) {
        return false;
    }

    redclaw::protocol::UacConsentEnvelopeV1 envelope;
    envelope.session_id = "router-session";
    envelope.token_id = grant.grant_token.token_id;
    envelope.uac_prompt_id = "prompt-allow";
    envelope.decision = redclaw::protocol::UacConsentDecisionV1::allow;

    const auto route = router.route(redclaw::protocol::serialize_uac_consent_envelope_v1(envelope));
    return expect_true(route.accepted, "router should accept valid allow message")
        && expect_true(route.applied_result.applied, "allow route should apply consent")
        && expect_true(
            broker.currentCapability("router-session") == redclaw::service::CapabilityLevel::kFullControl,
            "allow route should keep full-control capability");
}

bool test_router_rejects_session_mismatch() {
    std::uint64_t fake_now = 1'710'200'100ULL;
    redclaw::service::InMemoryPrivilegedControlBroker broker([&fake_now]() { return fake_now; });
    redclaw::service::InMemoryUacPromptLifecycleEventSource lifecycle_source;
    redclaw::service::InMemoryUacConsentActionTransport consent_transport;
    redclaw::service::HostServiceUacPromptRuntimeCoordinator coordinator(broker, lifecycle_source, consent_transport);
    redclaw::service::HostServiceUacConsentSignalProducer consent_producer(consent_transport, "router-session");
    redclaw::session::SessionUacConsentIngressRouter router(consent_producer, "router-session");

    const auto grant = broker.requestPrivilegedControl(make_request(fake_now, "router-challenge-2"));
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    lifecycle_source.emit_prompt_raised("router-session", "prompt-deny");
    if (!expect_true(coordinator.last_prompt_event_applied(), "prompt raised should be applied")) {
        return false;
    }

    redclaw::protocol::UacConsentEnvelopeV1 envelope;
    envelope.session_id = "other-session";
    envelope.token_id = grant.grant_token.token_id;
    envelope.uac_prompt_id = "prompt-deny";
    envelope.decision = redclaw::protocol::UacConsentDecisionV1::deny;

    const auto route = router.route(redclaw::protocol::serialize_uac_consent_envelope_v1(envelope));
    return expect_true(!route.accepted, "router must reject session mismatch")
        && expect_true(
            route.decision == redclaw::session::IncomingUacConsentDecision::rejected_session_mismatch,
            "session mismatch should map to rejected_session_mismatch");
}

bool test_router_applies_timeout_message_after_fail_close() {
    std::uint64_t fake_now = 1'710'200'200ULL;
    redclaw::service::InMemoryPrivilegedControlBroker broker([&fake_now]() { return fake_now; });
    redclaw::service::InMemoryUacPromptLifecycleEventSource lifecycle_source;
    redclaw::service::InMemoryUacConsentActionTransport consent_transport;
    redclaw::service::HostServiceUacPromptRuntimeCoordinator coordinator(broker, lifecycle_source, consent_transport);
    redclaw::service::HostServiceUacConsentSignalProducer consent_producer(consent_transport, "router-session");
    redclaw::session::SessionUacConsentIngressRouter router(consent_producer, "router-session");

    const auto grant = broker.requestPrivilegedControl(make_request(fake_now, "router-challenge-3"));
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    lifecycle_source.emit_prompt_raised("router-session", "prompt-timeout");
    lifecycle_source.emit_prompt_timeout("router-session", "prompt-timeout");

    redclaw::protocol::UacConsentEnvelopeV1 envelope;
    envelope.session_id = "router-session";
    envelope.token_id = grant.grant_token.token_id;
    envelope.uac_prompt_id = "prompt-timeout";
    envelope.decision = redclaw::protocol::UacConsentDecisionV1::timeout;

    const auto route = router.route(redclaw::protocol::serialize_uac_consent_envelope_v1(envelope));
    return expect_true(route.accepted, "router should accept timeout message")
        && expect_true(!route.applied_result.applied, "timeout consent should be blocked after fail-close")
        && expect_true(route.applied_result.error == redclaw::service::PrivilegedError::kPolicyDenied, "blocked timeout should report policy denied")
        && expect_true(
            broker.currentCapability("router-session") == redclaw::service::CapabilityLevel::kStandardControl,
            "timeout lifecycle should keep capability downgraded");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_router_applies_allow_message() && ok;
    ok = test_router_rejects_session_mismatch() && ok;
    ok = test_router_applies_timeout_message_after_fail_close() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_m07_session_uac_consent_ingress_router_integration_tests" << '\n';
    return 0;
}
