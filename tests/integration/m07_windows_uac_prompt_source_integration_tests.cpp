#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "redclaw/service/service_module.h"

namespace {

class FakeWindowsUacPromptEventAdapter final : public redclaw::service::IWindowsUacPromptEventAdapter {
public:
    void set_event_sink(redclaw::service::UacPromptLifecycleHandler sink) override {
        sink_ = std::move(sink);
    }

    bool start(std::string* error_detail) override {
        ++start_calls;
        if (fail_start) {
            if (error_detail != nullptr) {
                *error_detail = "forced_failure:uac_start";
            }
            return false;
        }

        started = true;
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool stop(std::string* error_detail) override {
        ++stop_calls;
        started = false;
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    void emit(const redclaw::service::UacPromptLifecycleEvent& event) const {
        if (!sink_) {
            return;
        }

        sink_(event);
    }

    bool fail_start = false;
    bool started = false;
    int start_calls = 0;
    int stop_calls = 0;

private:
    redclaw::service::UacPromptLifecycleHandler sink_;
};

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

redclaw::service::PrivilegedRequest make_request(std::uint64_t now, std::string challenge_id) {
    redclaw::service::PrivilegedRequest request;
    request.session_id = "uac-source-session";
    request.step_up_proof.operator_id = "uac-operator";
    request.step_up_proof.device_fingerprint = "uac-device";
    request.step_up_proof.challenge_id = std::move(challenge_id);
    request.step_up_proof.signed_proof = "proof";
    request.step_up_proof.issued_at_unix = now;
    return request;
}

bool test_windows_uac_source_start_failure_surfaces_error() {
    auto adapter = std::make_shared<FakeWindowsUacPromptEventAdapter>();
    adapter->fail_start = true;

    redclaw::service::WindowsUacPromptLifecycleEventSource source(adapter);

    const bool started = source.start_monitoring();
    return expect_true(!started, "start_monitoring should fail when adapter start fails")
        && expect_true(!source.running(), "source should remain stopped on start failure")
        && expect_true(source.last_error_detail() == "forced_failure:uac_start", "start failure detail should propagate");
}

bool test_windows_uac_source_raised_event_flows_through_coordinator() {
    std::uint64_t fake_now = 1'710'010'000ULL;
    auto adapter = std::make_shared<FakeWindowsUacPromptEventAdapter>();

    redclaw::service::InMemoryPrivilegedControlBroker broker([&fake_now]() { return fake_now; });
    redclaw::service::WindowsUacPromptLifecycleEventSource source(adapter);
    redclaw::service::InMemoryUacConsentActionTransport consent_transport;
    redclaw::service::HostServiceUacPromptRuntimeCoordinator coordinator(broker, source, consent_transport);

    if (!expect_true(source.start_monitoring(), "source should start monitoring")) {
        return false;
    }

    const auto grant = broker.requestPrivilegedControl(make_request(fake_now, "uac-source-challenge-1"));
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    adapter->emit({
        .session_id = "uac-source-session",
        .uac_prompt_id = "uac-source-prompt-1",
        .type = redclaw::service::UacPromptLifecycleEventType::kRaised,
        .reason_code = "raised",
    });

    if (!expect_true(coordinator.last_prompt_event_applied(), "raised prompt event should be applied")) {
        return false;
    }

    redclaw::service::UacConsentAction action;
    action.session_id = "uac-source-session";
    action.token_id = grant.grant_token.token_id;
    action.decision = redclaw::service::PrivilegedDecision::kAllow;
    action.uac_prompt_id = "uac-source-prompt-1";

    const auto result = consent_transport.submit(action);
    return expect_true(result.applied, "allow decision should apply after raised prompt")
        && expect_true(result.error == redclaw::service::PrivilegedError::kNone, "allow path should return no error")
        && expect_true(
            broker.currentCapability("uac-source-session") == redclaw::service::CapabilityLevel::kFullControl,
            "allow path should keep full-control capability")
        && expect_true(source.stop_monitoring(), "source stop should succeed");
}

bool test_windows_uac_source_unavailable_event_fail_closes() {
    std::uint64_t fake_now = 1'710'010'100ULL;
    auto adapter = std::make_shared<FakeWindowsUacPromptEventAdapter>();

    redclaw::service::InMemoryPrivilegedControlBroker broker([&fake_now]() { return fake_now; });
    redclaw::service::WindowsUacPromptLifecycleEventSource source(adapter);
    redclaw::service::InMemoryUacConsentActionTransport consent_transport;
    redclaw::service::HostServiceUacPromptRuntimeCoordinator coordinator(broker, source, consent_transport);

    if (!expect_true(source.start_monitoring(), "source should start monitoring")) {
        return false;
    }

    const auto grant = broker.requestPrivilegedControl(make_request(fake_now, "uac-source-challenge-2"));
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    adapter->emit({
        .session_id = "uac-source-session",
        .uac_prompt_id = "uac-source-prompt-2",
        .type = redclaw::service::UacPromptLifecycleEventType::kRaised,
        .reason_code = "raised",
    });

    if (!expect_true(coordinator.last_prompt_event_applied(), "raised prompt should be applied")) {
        return false;
    }

    adapter->emit({
        .session_id = "uac-source-session",
        .uac_prompt_id = "uac-source-prompt-2",
        .type = redclaw::service::UacPromptLifecycleEventType::kUnavailable,
        .reason_code = "secure_desktop_transport_lost",
    });

    if (!expect_true(coordinator.last_prompt_event_applied(), "unavailable event should be applied")) {
        return false;
    }

    if (!expect_true(
            broker.currentCapability("uac-source-session") == redclaw::service::CapabilityLevel::kStandardControl,
            "unavailable event should revoke full control")) {
        return false;
    }

    redclaw::service::UacConsentAction action;
    action.session_id = "uac-source-session";
    action.token_id = grant.grant_token.token_id;
    action.decision = redclaw::service::PrivilegedDecision::kAllow;
    action.uac_prompt_id = "uac-source-prompt-2";

    const auto result = consent_transport.submit(action);
    return expect_true(!result.applied, "consent should be blocked after unavailable fail-close")
        && expect_true(result.error == redclaw::service::PrivilegedError::kPolicyDenied, "blocked consent should report policy denied")
        && expect_true(source.stop_monitoring(), "source stop should succeed");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_windows_uac_source_start_failure_surfaces_error() && ok;
    ok = test_windows_uac_source_raised_event_flows_through_coordinator() && ok;
    ok = test_windows_uac_source_unavailable_event_fail_closes() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_m07_windows_uac_prompt_source_integration_tests" << '\n';
    return 0;
}
