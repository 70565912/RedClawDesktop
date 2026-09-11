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

bool test_callback_driven_source_bridges_runtime_state() {
    std::uint64_t fake_now = 1'710'002'100ULL;
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
    redclaw::service::CallbackDrivenHostServiceLifecycleEventSource source;
    redclaw::session::bind_host_service_lifecycle_event_source(source, dispatcher);

    redclaw::service::PrivilegedRequest request;
    request.session_id = "callback-source-session";
    request.step_up_proof.operator_id = "operator-callback-source";
    request.step_up_proof.device_fingerprint = "device-callback-source";
    request.step_up_proof.challenge_id = "challenge-callback-source";
    request.step_up_proof.signed_proof = "proof";
    request.step_up_proof.issued_at_unix = fake_now;

    if (!expect_true(source.start_host_service(), "callback source should start")) {
        return false;
    }

    const auto grant = orchestrator.broker().requestPrivilegedControl(request);
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    source.notify_secure_desktop_channel_ready(request.session_id);
    if (!expect_true(orchestrator.secure_backend_enabled(), "ready callback should enable secure backend")) {
        return false;
    }

    source.notify_secure_desktop_channel_lost(request.session_id);
    if (!expect_true(!orchestrator.secure_backend_enabled(), "lost callback should disable secure backend")) {
        return false;
    }

    source.notify_secure_desktop_channel_ready(request.session_id);
    if (!expect_true(orchestrator.secure_backend_enabled(), "ready callback should re-enable secure backend")) {
        return false;
    }

    source.notify_transport_disconnected(request.session_id);
    if (!expect_true(!orchestrator.secure_backend_enabled(), "disconnect callback should disable secure backend")) {
        return false;
    }

    if (!expect_true(!secure_channel_available, "disconnect should clear channel availability setter")) {
        return false;
    }

    if (!expect_true(
            orchestrator.broker().currentCapability(request.session_id) == redclaw::service::CapabilityLevel::kStandardControl,
            "disconnect should revoke full-control capability")) {
        return false;
    }

    if (!expect_true(source.stop_host_service(request.session_id), "callback source should stop")) {
        return false;
    }

    return expect_true(!source.running(), "callback source should report stopped");
}

}  // namespace

int main() {
    if (!test_callback_driven_source_bridges_runtime_state()) {
        return 1;
    }

    std::cout << "[PASS] redclaw_callback_driven_host_service_lifecycle_event_source_tests" << '\n';
    return 0;
}
