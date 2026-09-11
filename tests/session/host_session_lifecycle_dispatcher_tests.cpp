#include <cstdint>
#include <iostream>
#include <string>

#include "redclaw/input/input_module.h"
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

bool test_dispatcher_drives_orchestrator_lifecycle_states() {
    std::uint64_t fake_now = 1'710'001'500ULL;
    bool secure_channel_available = false;

    redclaw::input::InMemoryInputInjectorBackend user_backend;
    redclaw::input::InMemoryInputInjectorBackend secure_backend;

    redclaw::session::PrivilegedInputRuntimeOrchestrator orchestrator(
        user_backend,
        secure_backend,
        [&fake_now]() { return fake_now; },
        [&secure_channel_available](bool available) {
            secure_channel_available = available;
        });
    redclaw::session::HostSessionLifecycleDispatcher dispatcher(orchestrator);

    redclaw::service::PrivilegedRequest request;
    request.session_id = "session-dispatch";
    request.step_up_proof.operator_id = "operator-dispatch";
    request.step_up_proof.device_fingerprint = "device-dispatch";
    request.step_up_proof.challenge_id = "challenge-dispatch";
    request.step_up_proof.signed_proof = "proof";
    request.step_up_proof.issued_at_unix = fake_now;

    const auto grant = orchestrator.broker().requestPrivilegedControl(request);
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    if (!expect_true(!orchestrator.secure_backend_enabled(), "secure backend should remain disabled until channel ready")) {
        return false;
    }

    dispatcher.on_host_service_started();
    if (!expect_true(!orchestrator.secure_backend_enabled(), "service start should keep fail-closed baseline")) {
        return false;
    }

    dispatcher.on_secure_desktop_channel_ready();
    if (!expect_true(orchestrator.secure_backend_enabled(), "channel ready should enable secure backend when full-control is active")) {
        return false;
    }

    if (!expect_true(secure_channel_available, "channel setter should be true after ready signal")) {
        return false;
    }

    dispatcher.on_secure_desktop_channel_lost();
    if (!expect_true(!orchestrator.secure_backend_enabled(), "channel lost should disable secure backend")) {
        return false;
    }

    dispatcher.on_secure_desktop_channel_ready();
    if (!expect_true(orchestrator.secure_backend_enabled(), "channel ready should re-enable secure backend")) {
        return false;
    }

    const bool disconnected_revoked = dispatcher.on_transport_disconnected("session-dispatch");
    if (!expect_true(disconnected_revoked, "transport disconnect should revoke privileged capability for session")) {
        return false;
    }

    if (!expect_true(!orchestrator.secure_backend_enabled(), "transport disconnect should disable secure backend")) {
        return false;
    }

    if (!expect_true(!orchestrator.full_control_enabled(), "transport disconnect should clear full-control state")) {
        return false;
    }

    if (!expect_true(!secure_channel_available, "transport disconnect should clear secure channel setter state")) {
        return false;
    }

    if (!expect_true(
            orchestrator.broker().currentCapability("session-dispatch") == redclaw::service::CapabilityLevel::kStandardControl,
            "transport disconnect should downgrade broker capability to standard")) {
        return false;
    }

    const auto second_grant = orchestrator.broker().requestPrivilegedControl(request);
    if (!expect_true(!second_grant.accepted, "replay challenge should be rejected in second grant attempt")) {
        return false;
    }

    // Same session reconnect with a fresh challenge should be able to re-grant.
    request.step_up_proof.challenge_id = "challenge-dispatch-reconnect";
    const auto regrant = orchestrator.broker().requestPrivilegedControl(request);
    if (!expect_true(regrant.accepted, "fresh challenge should grant again after session disconnect revoke")) {
        return false;
    }

    dispatcher.on_secure_desktop_channel_ready();
    if (!expect_true(orchestrator.secure_backend_enabled(), "channel ready should enable after regrant")) {
        return false;
    }

    const bool stopping_revoked = dispatcher.on_host_service_stopping("session-dispatch");
    return expect_true(stopping_revoked, "service stopping should revoke privileged capability for session")
        && expect_true(!orchestrator.secure_backend_enabled(), "service stopping should fail-close secure backend")
        && expect_true(!orchestrator.full_control_enabled(), "service stopping should clear full-control state")
        && expect_true(!secure_channel_available, "service stopping should clear channel setter state");
}

}  // namespace

int main() {
    if (!test_dispatcher_drives_orchestrator_lifecycle_states()) {
        return 1;
    }

    std::cout << "[PASS] redclaw_session_host_session_lifecycle_dispatcher_tests" << '\n';
    return 0;
}
