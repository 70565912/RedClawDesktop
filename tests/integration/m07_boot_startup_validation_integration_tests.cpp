#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "redclaw/service/service_module.h"

namespace {

class FakeWindowsServiceControlAdapter final : public redclaw::service::IWindowsServiceControlAdapter {
public:
    bool install(const redclaw::service::ServiceInstallConfig& config, std::string* error_detail) override {
        operations.push_back("install:" + config.service_name);
        installed_configs.push_back(config);
        if (fail_install) {
            if (error_detail != nullptr) {
                *error_detail = "forced_failure:install:" + config.service_name;
            }
            return false;
        }

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
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool uninstall(std::string_view service_name, std::string* error_detail) override {
        operations.push_back("uninstall:" + std::string(service_name));
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool configure_environment(
        std::string_view service_name,
        const std::unordered_map<std::string, std::string>& environment_overrides,
        std::string* error_detail) override {
        operations.push_back("configure_environment:" + std::string(service_name));
        (void)environment_overrides;
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    bool fail_install = false;
    bool fail_start = false;
    std::vector<redclaw::service::ServiceInstallConfig> installed_configs;
    std::vector<std::string> operations;
};

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_boot_install_uses_auto_start_policy() {
    auto adapter = std::make_shared<FakeWindowsServiceControlAdapter>();
    redclaw::service::WindowsServiceLifecycleWrapper wrapper(adapter);

    redclaw::service::ServiceInstallConfig config;
    config.service_name = "RedClawHostService";
    config.display_name = "RedClaw Host Service";
    config.binary_path = "C:/Program Files/RedClaw/redclaw_host_service.exe";
    config.auto_start = true;
    config.account_name = "LocalSystem";

    const auto install_result = wrapper.install(config);
    if (!expect_true(install_result == redclaw::service::ServiceLifecycleError::kNone, "boot install should succeed")) {
        return false;
    }

    if (!expect_true(!adapter->installed_configs.empty(), "adapter should capture one install config")) {
        return false;
    }

    const auto& installed = adapter->installed_configs.front();
    return expect_true(installed.auto_start, "boot install should preserve auto_start=true")
        && expect_true(installed.service_name == "RedClawHostService", "service name should be forwarded")
        && expect_true(installed.display_name == "RedClaw Host Service", "display name should be forwarded");
}

bool test_boot_runtime_bridge_start_stop_path() {
    auto adapter = std::make_shared<FakeWindowsServiceControlAdapter>();
    redclaw::service::WindowsServiceLifecycleWrapper wrapper(adapter);
    redclaw::service::CallbackDrivenHostServiceLifecycleEventSource source;
    redclaw::service::HostServiceRuntimeLifecycleBridge bridge(wrapper, source);

    bool saw_started = false;
    std::string stopping_session_id;
    source.set_events({
        .on_host_service_started = [&saw_started]() {
            saw_started = true;
        },
        .on_host_service_stopping = [&stopping_session_id](std::string_view session_id) {
            stopping_session_id = std::string(session_id);
        },
    });

    const auto start_result = bridge.start("RedClawHostService");
    if (!expect_true(start_result == redclaw::service::ServiceLifecycleError::kNone, "bridge start should succeed")) {
        return false;
    }

    if (!expect_true(source.running(), "event source should be running after bridge start")) {
        return false;
    }

    if (!expect_true(saw_started, "host service started callback should fire")) {
        return false;
    }

    const auto stop_result = bridge.stop("RedClawHostService", "boot-session");
    return expect_true(stop_result == redclaw::service::ServiceLifecycleError::kNone, "bridge stop should succeed")
        && expect_true(!source.running(), "event source should stop after bridge stop")
        && expect_true(stopping_session_id == "boot-session", "stop callback should include session id")
        && expect_true(adapter->operations.size() >= 2, "adapter should receive start and stop operations")
        && expect_true(adapter->operations[0] == "start:RedClawHostService", "first operation should be service start")
        && expect_true(adapter->operations[1] == "stop:RedClawHostService", "second operation should be service stop");
}

bool test_boot_start_failure_surfaces_error() {
    auto adapter = std::make_shared<FakeWindowsServiceControlAdapter>();
    adapter->fail_start = true;

    redclaw::service::WindowsServiceLifecycleWrapper wrapper(adapter);
    redclaw::service::CallbackDrivenHostServiceLifecycleEventSource source;
    redclaw::service::HostServiceRuntimeLifecycleBridge bridge(wrapper, source);

    const auto start_result = bridge.start("RedClawHostService");
    return expect_true(start_result == redclaw::service::ServiceLifecycleError::kCommandFailed, "boot start failure should propagate")
        && expect_true(
            bridge.last_error_detail().find("forced_failure:start:RedClawHostService") == 0,
            "bridge error should preserve adapter start failure detail")
        && expect_true(!source.running(), "event source should remain stopped when start fails");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_boot_install_uses_auto_start_policy() && ok;
    ok = test_boot_runtime_bridge_start_stop_path() && ok;
    ok = test_boot_start_failure_surfaces_error() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_m07_boot_startup_validation_integration_tests" << '\n';
    return 0;
}
