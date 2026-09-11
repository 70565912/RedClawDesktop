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

bool test_service_emitter_drives_session_dispatcher_chain() {
    std::uint64_t fake_now = 1'710'002'000ULL;
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
    redclaw::service::InMemoryHostServiceLifecycleEmitter emitter;

    redclaw::session::bind_host_service_lifecycle_event_source(emitter, dispatcher);

    const std::string session_id = "session-event-source";
    redclaw::input::InputEvent secure_event;
    secure_event.target = redclaw::input::InputTarget::kSecureDesktop;
    secure_event.type = redclaw::input::InputEventType::kMouseButtonDown;
    secure_event.mouse_button = redclaw::input::MouseButton::kLeft;

    const bool started = emitter.start_host_service();
    if (!expect_true(started, "service emitter should start once")) {
        return false;
    }

    redclaw::service::PrivilegedRequest request;
    request.session_id = session_id;
    request.step_up_proof.operator_id = "operator-event-source";
    request.step_up_proof.device_fingerprint = "device-event-source";
    request.step_up_proof.challenge_id = "challenge-event-source";
    request.step_up_proof.signed_proof = "proof";
    request.step_up_proof.issued_at_unix = fake_now;

    const auto grant = orchestrator.broker().requestPrivilegedControl(request);
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    emitter.mark_secure_desktop_channel_ready(session_id);
    if (!expect_true(orchestrator.secure_backend_enabled(), "channel ready event should enable secure backend")) {
        return false;
    }

    if (!expect_true(secure_channel_available, "secure channel setter should be true after ready event")) {
        return false;
    }

    const auto injected_before_loss = orchestrator.adapter().inject_event(secure_event);
    if (!expect_true(injected_before_loss.injected, "secure desktop event should inject after grant and channel ready")) {
        return false;
    }

    if (!expect_true(secure_backend.injected_events().size() == 1, "secure backend should receive one injected event")) {
        return false;
    }

    emitter.mark_secure_desktop_channel_lost(session_id);
    if (!expect_true(!orchestrator.secure_backend_enabled(), "channel lost should disable secure backend")) {
        return false;
    }

    const auto blocked_after_channel_loss = orchestrator.adapter().inject_event(secure_event);
    if (!expect_true(!blocked_after_channel_loss.injected, "secure desktop should be blocked when secure channel is lost")) {
        return false;
    }

    if (!expect_true(
            blocked_after_channel_loss.reason == "secure_desktop_requires_privileged_mode",
            "channel-lost deny should fail closed via policy gate")) {
        return false;
    }

    if (!expect_true(secure_backend.injected_events().size() == 1, "blocked channel-lost event must not reach secure backend")) {
        return false;
    }

    emitter.mark_transport_disconnected(session_id);
    if (!expect_true(!orchestrator.secure_backend_enabled(), "disconnect event should disable secure backend")) {
        return false;
    }

    if (!expect_true(
            orchestrator.broker().currentCapability(session_id) == redclaw::service::CapabilityLevel::kStandardControl,
            "disconnect event should revoke broker capability")) {
        return false;
    }

    if (!expect_true(!secure_channel_available, "disconnect event should clear secure channel availability")) {
        return false;
    }

    const auto blocked_after_disconnect = orchestrator.adapter().inject_event(secure_event);
    if (!expect_true(!blocked_after_disconnect.injected, "secure desktop should stay blocked after disconnect revoke")) {
        return false;
    }

    if (!expect_true(
            blocked_after_disconnect.reason == "secure_desktop_requires_privileged_mode",
            "disconnect deny should surface policy revoke reason")) {
        return false;
    }

    if (!expect_true(secure_backend.injected_events().size() == 1, "disconnect-blocked event must not reach secure backend")) {
        return false;
    }

    request.step_up_proof.challenge_id = "challenge-event-source-2";
    const auto regrant = orchestrator.broker().requestPrivilegedControl(request);
    if (!expect_true(regrant.accepted, "fresh challenge should re-grant after disconnect")) {
        return false;
    }

    emitter.mark_secure_desktop_channel_ready(session_id);
    if (!expect_true(orchestrator.secure_backend_enabled(), "channel ready should re-enable after re-grant")) {
        return false;
    }

    const bool stopped = emitter.stop_host_service(session_id);
    return expect_true(stopped, "service emitter should stop")
        && expect_true(!emitter.running(), "service emitter should report stopped")
        && expect_true(!orchestrator.secure_backend_enabled(), "service stop should fail-close secure backend")
        && expect_true(
            orchestrator.broker().currentCapability(session_id) == redclaw::service::CapabilityLevel::kStandardControl,
            "service stop should revoke broker capability");
}

}  // namespace

int main() {
    if (!test_service_emitter_drives_session_dispatcher_chain()) {
        return 1;
    }

    std::cout << "[PASS] redclaw_service_session_event_source_integration_tests" << '\n';
    return 0;
}
