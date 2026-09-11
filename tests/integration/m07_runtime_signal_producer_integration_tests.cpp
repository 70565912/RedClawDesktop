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

bool test_signal_producer_binds_runtime_signals_to_bridge_notifications() {
    std::uint64_t fake_now = 1'710'002'280ULL;
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

    redclaw::service::HostServiceSessionSignalProducer producer(
        bridge,
        "RedClawHostService",
        "producer-session");

    const auto start_result = producer.start_host_service();
    if (!expect_true(start_result == redclaw::service::ServiceLifecycleError::kNone, "producer start should succeed")) {
        return false;
    }

    redclaw::service::PrivilegedRequest request;
    request.session_id = "producer-session";
    request.step_up_proof.operator_id = "producer-operator";
    request.step_up_proof.device_fingerprint = "producer-device";
    request.step_up_proof.challenge_id = "producer-challenge";
    request.step_up_proof.signed_proof = "proof";
    request.step_up_proof.issued_at_unix = fake_now;

    const auto grant = orchestrator.broker().requestPrivilegedControl(request);
    if (!expect_true(grant.accepted, "privileged grant should succeed")) {
        return false;
    }

    producer.on_secure_desktop_channel_ready();
    if (!expect_true(orchestrator.secure_backend_enabled(), "ready signal should enable secure backend")) {
        return false;
    }

    if (!expect_true(secure_channel_available, "ready signal should set secure channel availability true")) {
        return false;
    }

    producer.on_secure_desktop_channel_lost();
    if (!expect_true(!orchestrator.secure_backend_enabled(), "lost signal should disable secure backend")) {
        return false;
    }

    producer.on_secure_desktop_channel_ready();
    if (!expect_true(orchestrator.secure_backend_enabled(), "ready after lost should re-enable secure backend")) {
        return false;
    }

    producer.on_transport_disconnected();
    if (!expect_true(!orchestrator.secure_backend_enabled(), "disconnect signal should disable secure backend")) {
        return false;
    }

    if (!expect_true(
            orchestrator.broker().currentCapability("producer-session") == redclaw::service::CapabilityLevel::kStandardControl,
            "disconnect should revoke privileged capability")) {
        return false;
    }

    producer.on_secure_desktop_channel_ready();
    if (!expect_true(!orchestrator.secure_backend_enabled(), "ready after disconnect should stay blocked until re-grant")) {
        return false;
    }

    request.step_up_proof.challenge_id = "producer-challenge-2";
    const auto regrant = orchestrator.broker().requestPrivilegedControl(request);
    if (!expect_true(regrant.accepted, "fresh challenge should re-grant after disconnect")) {
        return false;
    }

    producer.on_secure_desktop_channel_ready();
    if (!expect_true(orchestrator.secure_backend_enabled(), "ready should re-enable secure backend after re-grant")) {
        return false;
    }

    const auto stop_result = producer.stop_host_service();
    if (!expect_true(stop_result == redclaw::service::ServiceLifecycleError::kNone, "producer stop should succeed")) {
        return false;
    }

    return expect_true(!source.running(), "event source should be stopped after producer stop")
        && expect_true(!orchestrator.secure_backend_enabled(), "stop should fail-close secure backend")
        && expect_true(
            orchestrator.broker().currentCapability("producer-session") == redclaw::service::CapabilityLevel::kStandardControl,
            "stop should keep capability revoked")
        && expect_true(adapter->operations.size() == 2, "adapter should record start and stop")
        && expect_true(adapter->operations[0] == "start:RedClawHostService", "first adapter operation should be service start")
        && expect_true(adapter->operations[1] == "stop:RedClawHostService", "second adapter operation should be service stop");
}

bool test_signal_producer_surfaces_start_failures() {
    auto adapter = std::make_shared<FakeWindowsServiceControlAdapter>();
    adapter->fail_start = true;

    redclaw::service::WindowsServiceLifecycleWrapper wrapper(adapter);
    redclaw::service::CallbackDrivenHostServiceLifecycleEventSource source;
    redclaw::service::HostServiceRuntimeLifecycleBridge bridge(wrapper, source);
    redclaw::service::HostServiceSessionSignalProducer producer(
        bridge,
        "RedClawHostService",
        "producer-session");

    const auto start_result = producer.start_host_service();
    return expect_true(start_result == redclaw::service::ServiceLifecycleError::kCommandFailed, "start failure should propagate")
        && expect_true(
            producer.last_error_detail().find("forced_failure:start:RedClawHostService") == 0,
            "producer last_error_detail should include adapter start failure")
        && expect_true(!source.running(), "event source should remain stopped when start fails");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_signal_producer_binds_runtime_signals_to_bridge_notifications() && ok;
    ok = test_signal_producer_surfaces_start_failures() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_m07_runtime_signal_producer_integration_tests" << '\n';
    return 0;
}
