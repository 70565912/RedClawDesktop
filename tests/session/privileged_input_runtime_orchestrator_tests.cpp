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

redclaw::service::PrivilegedRequest make_request(
    std::string session_id,
    std::string challenge_id,
    std::uint64_t issued_at_unix) {
    redclaw::service::PrivilegedRequest request;
    request.session_id = std::move(session_id);
    request.step_up_proof.operator_id = "operator-orchestrator";
    request.step_up_proof.device_fingerprint = "device-orchestrator";
    request.step_up_proof.challenge_id = std::move(challenge_id);
    request.step_up_proof.signed_proof = "proof";
    request.step_up_proof.issued_at_unix = issued_at_unix;
    return request;
}

redclaw::input::InputEvent make_secure_click_event() {
    redclaw::input::InputEvent secure_event;
    secure_event.target = redclaw::input::InputTarget::kSecureDesktop;
    secure_event.type = redclaw::input::InputEventType::kMouseButtonDown;
    secure_event.mouse_button = redclaw::input::MouseButton::kLeft;
    return secure_event;
}

bool test_orchestrator_syncs_capability_to_secure_input_runtime() {
    std::uint64_t fake_now = 1'710'001'200ULL;
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

    redclaw::input::InputEvent secure_event;
    secure_event.target = redclaw::input::InputTarget::kSecureDesktop;
    secure_event.type = redclaw::input::InputEventType::kMouseButtonDown;
    secure_event.mouse_button = redclaw::input::MouseButton::kLeft;

    const auto before_grant = orchestrator.adapter().inject_event(secure_event);
    if (!expect_true(!before_grant.injected, "secure desktop should be blocked before grant")) {
        return false;
    }

    if (!expect_true(
            before_grant.reason == "secure_desktop_requires_privileged_mode",
            "before grant deny reason should match")) {
        return false;
    }

    redclaw::service::PrivilegedRequest request;
    request.session_id = "session-orchestrator";
    request.step_up_proof.operator_id = "operator-orchestrator";
    request.step_up_proof.device_fingerprint = "device-orchestrator";
    request.step_up_proof.challenge_id = "challenge-orchestrator";
    request.step_up_proof.signed_proof = "proof";
    request.step_up_proof.issued_at_unix = fake_now;

    const auto grant = orchestrator.broker().requestPrivilegedControl(request);
    if (!expect_true(grant.accepted, "privileged request should be granted")) {
        return false;
    }

    const auto granted_without_channel = orchestrator.adapter().inject_event(secure_event);
    if (!expect_true(!granted_without_channel.injected, "secure desktop should stay blocked until channel is ready")) {
        return false;
    }

    if (!expect_true(!orchestrator.secure_backend_enabled(), "secure backend should remain disabled before channel ready")) {
        return false;
    }

    if (!expect_true(!secure_channel_available, "secure channel setter should remain false before readiness")) {
        return false;
    }

    orchestrator.set_secure_desktop_channel_available(true);

    if (!expect_true(orchestrator.secure_backend_enabled(), "secure backend should be enabled after grant and channel ready")) {
        return false;
    }

    if (!expect_true(orchestrator.secure_desktop_channel_available(), "orchestrator should report secure channel available")) {
        return false;
    }

    if (!expect_true(secure_channel_available, "secure channel setter should be true after readiness")) {
        return false;
    }

    const auto after_grant = orchestrator.adapter().inject_event(secure_event);
    if (!expect_true(after_grant.injected, "secure desktop should inject after grant")) {
        return false;
    }

    const bool revoked = orchestrator.broker().revokePrivilegedControl("session-orchestrator", "orchestrator_test_revoke");
    if (!expect_true(revoked, "revoke should succeed")) {
        return false;
    }

    if (!expect_true(!orchestrator.full_control_enabled(), "full-control flag should be false after revoke")) {
        return false;
    }

    orchestrator.on_secure_desktop_channel_lost();
    if (!expect_true(!orchestrator.secure_desktop_channel_available(), "channel should be unavailable after lifecycle loss signal")) {
        return false;
    }

    const auto after_revoke = orchestrator.adapter().inject_event(secure_event);
    if (!expect_true(!after_revoke.injected, "secure desktop should be blocked after revoke")) {
        return false;
    }

    orchestrator.on_secure_desktop_channel_ready();
    if (!expect_true(orchestrator.secure_desktop_channel_available(), "channel should be available after lifecycle ready signal")) {
        return false;
    }

    const bool disconnected_revoked = orchestrator.on_session_disconnected("session-orchestrator");
    if (!expect_true(disconnected_revoked, "session disconnect should revoke privileged capability")) {
        return false;
    }
    if (!expect_true(!orchestrator.full_control_enabled(), "full-control should be cleared on session disconnect")) {
        return false;
    }

    if (!expect_true(!orchestrator.secure_desktop_channel_available(), "channel should be cleared on session disconnect")) {
        return false;
    }

    return expect_true(
            after_revoke.reason == "secure_desktop_requires_privileged_mode",
            "after revoke deny reason should match")
        && expect_true(!orchestrator.secure_backend_enabled(), "secure backend should be disabled after revoke")
        && expect_true(!orchestrator.secure_desktop_channel_available(), "orchestrator should report secure channel unavailable")
        && expect_true(!secure_channel_available, "secure channel setter should be false after channel down")
        && expect_true(secure_backend.injected_events().size() == 1, "secure backend should only receive one event");
}

bool test_ready_disconnect_ready_without_regrant_stays_blocked() {
    std::uint64_t fake_now = 1'710'001'260ULL;
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

    const std::string session_id = "session-race-ready-disconnect-ready";
    auto request = make_request(session_id, "challenge-race-1", fake_now);
    const auto grant = orchestrator.broker().requestPrivilegedControl(request);
    if (!expect_true(grant.accepted, "grant should succeed before sequence test")) {
        return false;
    }

    const auto secure_event = make_secure_click_event();
    orchestrator.on_secure_desktop_channel_ready();
    if (!expect_true(orchestrator.secure_backend_enabled(), "ready should enable backend while full-control is active")) {
        return false;
    }

    const auto injected_before_disconnect = orchestrator.adapter().inject_event(secure_event);
    if (!expect_true(injected_before_disconnect.injected, "secure event should inject before disconnect")) {
        return false;
    }

    const bool revoked = orchestrator.on_session_disconnected(session_id);
    if (!expect_true(revoked, "disconnect should revoke full-control for active session")) {
        return false;
    }

    orchestrator.on_secure_desktop_channel_ready();
    const auto blocked_after_ready = orchestrator.adapter().inject_event(secure_event);
    return expect_true(!orchestrator.full_control_enabled(), "full-control should remain cleared after disconnect")
        && expect_true(orchestrator.secure_desktop_channel_available(), "ready signal can mark channel available after disconnect")
        && expect_true(!orchestrator.secure_backend_enabled(), "backend must stay disabled without regrant")
        && expect_true(secure_channel_available, "channel setter should reflect latest ready signal")
        && expect_true(!blocked_after_ready.injected, "secure event must be blocked until regrant")
        && expect_true(
            blocked_after_ready.reason == "secure_desktop_requires_privileged_mode",
            "blocked event reason should remain policy fail-close")
        && expect_true(secure_backend.injected_events().size() == 1, "blocked post-disconnect event must not reach backend");
}

bool test_disconnect_then_ready_requires_fresh_regrant() {
    std::uint64_t fake_now = 1'710'001'320ULL;
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

    const std::string session_id = "session-race-disconnect-ready";
    auto request = make_request(session_id, "challenge-race-2", fake_now);
    const auto initial_grant = orchestrator.broker().requestPrivilegedControl(request);
    if (!expect_true(initial_grant.accepted, "initial grant should succeed")) {
        return false;
    }

    const auto secure_event = make_secure_click_event();
    orchestrator.on_secure_desktop_channel_ready();
    if (!expect_true(orchestrator.secure_backend_enabled(), "backend should enable after initial ready")) {
        return false;
    }

    const bool disconnected = orchestrator.on_session_disconnected(session_id);
    if (!expect_true(disconnected, "disconnect should revoke full-control")) {
        return false;
    }

    orchestrator.on_secure_desktop_channel_ready();
    const auto blocked_without_regrant = orchestrator.adapter().inject_event(secure_event);
    if (!expect_true(!blocked_without_regrant.injected, "ready after disconnect must remain blocked before regrant")) {
        return false;
    }

    if (!expect_true(!orchestrator.secure_backend_enabled(), "backend should remain disabled before regrant")) {
        return false;
    }

    request = make_request(session_id, "challenge-race-3", fake_now);
    const auto regrant = orchestrator.broker().requestPrivilegedControl(request);
    if (!expect_true(regrant.accepted, "fresh challenge should regrant after disconnect")) {
        return false;
    }

    const auto injected_after_regrant = orchestrator.adapter().inject_event(secure_event);
    return expect_true(orchestrator.full_control_enabled(), "full-control should be restored after fresh regrant")
        && expect_true(orchestrator.secure_desktop_channel_available(), "channel should remain available after ready signal")
        && expect_true(orchestrator.secure_backend_enabled(), "backend should re-enable after regrant with ready channel")
        && expect_true(secure_channel_available, "channel setter should remain true through regrant")
        && expect_true(injected_after_regrant.injected, "secure event should inject after fresh regrant");
}

bool test_ready_lost_ready_toggles_backend_with_active_capability() {
    std::uint64_t fake_now = 1'710'001'380ULL;
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

    const std::string session_id = "session-race-ready-lost-ready";
    const auto grant = orchestrator.broker().requestPrivilegedControl(
        make_request(session_id, "challenge-race-4", fake_now));
    if (!expect_true(grant.accepted, "grant should succeed for lost-ready sequence")) {
        return false;
    }

    orchestrator.on_secure_desktop_channel_ready();
    if (!expect_true(orchestrator.secure_backend_enabled(), "first ready should enable backend")) {
        return false;
    }

    orchestrator.on_secure_desktop_channel_lost();
    if (!expect_true(!orchestrator.secure_backend_enabled(), "lost should disable backend")) {
        return false;
    }

    const auto secure_event = make_secure_click_event();
    const auto blocked_after_lost = orchestrator.adapter().inject_event(secure_event);
    if (!expect_true(!blocked_after_lost.injected, "secure event should be blocked while channel is lost")) {
        return false;
    }

    orchestrator.on_secure_desktop_channel_ready();
    const auto injected_after_recovery = orchestrator.adapter().inject_event(secure_event);
    return expect_true(orchestrator.full_control_enabled(), "full-control should remain active across channel lost/ready")
        && expect_true(orchestrator.secure_desktop_channel_available(), "channel should be available after second ready")
        && expect_true(orchestrator.secure_backend_enabled(), "second ready should re-enable backend")
        && expect_true(secure_channel_available, "channel setter should report true after second ready")
        && expect_true(injected_after_recovery.injected, "secure event should inject after channel recovery")
        && expect_true(secure_backend.injected_events().size() == 1, "only recovered event should reach secure backend");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_orchestrator_syncs_capability_to_secure_input_runtime() && ok;
    ok = test_ready_disconnect_ready_without_regrant_stays_blocked() && ok;
    ok = test_disconnect_then_ready_requires_fresh_regrant() && ok;
    ok = test_ready_lost_ready_toggles_backend_with_active_capability() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_session_privileged_input_runtime_orchestrator_tests" << '\n';
    return 0;
}
