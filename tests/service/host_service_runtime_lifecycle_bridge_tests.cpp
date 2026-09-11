#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "redclaw/input/input_module.h"
#include "redclaw/service/service_module.h"
#include "redclaw/session/session_module.h"

namespace {

class FakeWindowsServiceControlAdapter final : public redclaw::service::IWindowsServiceControlAdapter {
public:
    bool install(const redclaw::service::ServiceInstallConfig& config, std::string* error_detail) override {
        (void)config;
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool start(std::string_view service_name, std::string* error_detail) override {
        operations.push_back("start:" + std::string(service_name));
        if (fail_start) {
            if (error_detail != nullptr) {
                *error_detail = "forced_failure:start:" + std::string(service_name);
            }
            return false;
        }

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool stop(std::string_view service_name, std::string* error_detail) override {
        operations.push_back("stop:" + std::string(service_name));
        if (fail_stop) {
            if (error_detail != nullptr) {
                *error_detail = "forced_failure:stop:" + std::string(service_name);
            }
            return false;
        }

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool uninstall(std::string_view service_name, std::string* error_detail) override {
        (void)service_name;
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool configure_environment(
        std::string_view service_name,
        const std::unordered_map<std::string, std::string>& environment_overrides,
        std::string* error_detail) override {
        (void)service_name;
        (void)environment_overrides;
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool fail_start = false;
    bool fail_stop = false;
    std::vector<std::string> operations;
};

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_bridge_start_and_signal_forwarding_drive_runtime_gate() {
    std::uint64_t fake_now = 1'710'002'220ULL;
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

    auto adapter = std::make_shared<FakeWindowsServiceControlAdapter>();
    redclaw::service::WindowsServiceLifecycleWrapper wrapper(adapter);
    redclaw::service::CallbackDrivenHostServiceLifecycleEventSource source;
    redclaw::session::bind_host_service_lifecycle_event_source(source, dispatcher);
    redclaw::service::HostServiceRuntimeLifecycleBridge bridge(wrapper, source);

    redclaw::service::PrivilegedRequest request;
    request.session_id = "bridge-session";
    request.step_up_proof.operator_id = "bridge-operator";
    request.step_up_proof.device_fingerprint = "bridge-device";
    request.step_up_proof.challenge_id = "bridge-challenge";
    request.step_up_proof.signed_proof = "proof";
    request.step_up_proof.issued_at_unix = fake_now;

    const auto grant = orchestrator.broker().requestPrivilegedControl(request);
    if (!expect_true(grant.accepted, "grant should succeed before channel ready")) {
        return false;
    }

    const auto start_result = bridge.start("RedClawHostService");
    if (!expect_true(start_result == redclaw::service::ServiceLifecycleError::kNone, "bridge start should succeed")) {
        return false;
    }

    bridge.notify_secure_desktop_channel_ready(request.session_id);
    if (!expect_true(orchestrator.secure_backend_enabled(), "ready signal should enable secure backend")) {
        return false;
    }

    bridge.notify_transport_disconnected(request.session_id);
    if (!expect_true(!orchestrator.secure_backend_enabled(), "disconnect signal should disable secure backend")) {
        return false;
    }

    if (!expect_true(!secure_channel_available, "disconnect should clear channel availability")) {
        return false;
    }

    bridge.notify_secure_desktop_channel_ready(request.session_id);
    if (!expect_true(!orchestrator.secure_backend_enabled(), "ready after disconnect should remain blocked until re-grant")) {
        return false;
    }

    return expect_true(
        orchestrator.broker().currentCapability(request.session_id) == redclaw::service::CapabilityLevel::kStandardControl,
        "disconnect should revoke privileged capability");
}

bool test_bridge_surfaces_wrapper_failures() {
    auto adapter = std::make_shared<FakeWindowsServiceControlAdapter>();
    adapter->fail_start = true;

    redclaw::service::WindowsServiceLifecycleWrapper wrapper(adapter);
    redclaw::service::CallbackDrivenHostServiceLifecycleEventSource source;
    redclaw::service::HostServiceRuntimeLifecycleBridge bridge(wrapper, source);

    const auto start_result = bridge.start("RedClawHostService");
    return expect_true(start_result == redclaw::service::ServiceLifecycleError::kCommandFailed, "failed wrapper start should propagate")
        && expect_true(bridge.last_error_detail().find("forced_failure:start:RedClawHostService") == 0, "bridge error detail should include start failure")
        && expect_true(!source.running(), "event source should not start when wrapper start fails");
}

bool test_bridge_stop_fail_closes_event_source_before_wrapper_stop() {
    auto adapter = std::make_shared<FakeWindowsServiceControlAdapter>();
    redclaw::service::WindowsServiceLifecycleWrapper wrapper(adapter);
    redclaw::service::CallbackDrivenHostServiceLifecycleEventSource source;
    redclaw::service::HostServiceRuntimeLifecycleBridge bridge(wrapper, source);

    const auto start_result = bridge.start("RedClawHostService");
    if (!expect_true(start_result == redclaw::service::ServiceLifecycleError::kNone, "bridge start should succeed")) {
        return false;
    }

    adapter->fail_stop = true;
    const auto stop_result = bridge.stop("RedClawHostService", "session-stop");
    return expect_true(stop_result == redclaw::service::ServiceLifecycleError::kCommandFailed, "stop failure should propagate")
        && expect_true(!source.running(), "event source should be fail-closed even when wrapper stop fails")
        && expect_true(bridge.last_error_detail().find("forced_failure:stop:RedClawHostService") == 0, "bridge error detail should include stop failure");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_bridge_start_and_signal_forwarding_drive_runtime_gate() && ok;
    ok = test_bridge_surfaces_wrapper_failures() && ok;
    ok = test_bridge_stop_fail_closes_event_source_before_wrapper_stop() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_host_service_runtime_lifecycle_bridge_tests" << '\n';
    return 0;
}
