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
        if (fail_operation == "install") {
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
        if (fail_operation == "start") {
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
        if (fail_operation == "stop") {
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
        operations.push_back("uninstall:" + std::string(service_name));
        if (fail_operation == "uninstall") {
            if (error_detail != nullptr) {
                *error_detail = "forced_failure:uninstall:" + std::string(service_name);
            }
            return false;
        }

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
        for (const auto& [key, value] : environment_overrides) {
            operations.push_back("env:" + key + "=" + value);
        }

        if (fail_operation == "configure_environment") {
            if (error_detail != nullptr) {
                *error_detail = "forced_failure:configure_environment:" + std::string(service_name);
            }
            return false;
        }

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

    std::string fail_operation;
    std::vector<std::string> operations;
};

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_invalid_config_fails_fast() {
    auto adapter = std::make_shared<FakeWindowsServiceControlAdapter>();
    redclaw::service::WindowsServiceLifecycleWrapper wrapper(adapter);

    redclaw::service::ServiceInstallConfig invalid_config;
    invalid_config.service_name = "";
    invalid_config.display_name = "RedClaw Host Service";
    invalid_config.binary_path = "C:/Program Files/RedClaw/redclaw_host_service.exe";

    const auto result = wrapper.install(invalid_config);
    return expect_true(result == redclaw::service::ServiceLifecycleError::kInvalidConfig, "invalid config should fail")
        && expect_true(wrapper.last_error_detail() == "invalid_service_install_config", "invalid config error detail should match");
}

bool test_command_failure_propagates_error_detail() {
    auto adapter = std::make_shared<FakeWindowsServiceControlAdapter>();
    adapter->fail_operation = "start";
    redclaw::service::WindowsServiceLifecycleWrapper wrapper(adapter);

    const auto result = wrapper.start("RedClawHostService");
    return expect_true(result == redclaw::service::ServiceLifecycleError::kCommandFailed, "failed command should map to command failed")
        && expect_true(
            wrapper.last_error_detail().find("forced_failure:start:RedClawHostService") == 0,
            "failure detail should include command context");
}

bool test_install_start_stop_uninstall_emits_expected_operations() {
    auto adapter = std::make_shared<FakeWindowsServiceControlAdapter>();
    redclaw::service::WindowsServiceLifecycleWrapper wrapper(adapter);

    redclaw::service::ServiceInstallConfig config;
    config.service_name = "RedClawHostService";
    config.display_name = "RedClaw Host Service";
    config.binary_path = "C:/Program Files/RedClaw/redclaw_host_service.exe";
    config.auto_start = true;
    config.account_name = "LocalSystem";

    const auto install_result = wrapper.install(config);
    const auto start_result = wrapper.start(config.service_name);
    const auto stop_result = wrapper.stop(config.service_name);
    const auto uninstall_result = wrapper.uninstall(config.service_name);

    if (!expect_true(install_result == redclaw::service::ServiceLifecycleError::kNone, "install should succeed")) {
        return false;
    }

    if (!expect_true(start_result == redclaw::service::ServiceLifecycleError::kNone, "start should succeed")) {
        return false;
    }

    if (!expect_true(stop_result == redclaw::service::ServiceLifecycleError::kNone, "stop should succeed")) {
        return false;
    }

    if (!expect_true(uninstall_result == redclaw::service::ServiceLifecycleError::kNone, "uninstall should succeed")) {
        return false;
    }

    const bool saw_install = adapter->operations.size() >= 1 && adapter->operations[0] == "install:RedClawHostService";
    const bool saw_start = adapter->operations.size() >= 2 && adapter->operations[1] == "start:RedClawHostService";
    const bool saw_stop = adapter->operations.size() >= 3 && adapter->operations[2] == "stop:RedClawHostService";
    const bool saw_uninstall = adapter->operations.size() >= 4 && adapter->operations[3] == "uninstall:RedClawHostService";

    return expect_true(saw_install, "first operation should be install")
        && expect_true(saw_start, "second operation should be start")
        && expect_true(saw_stop, "third operation should be stop")
        && expect_true(saw_uninstall, "fourth operation should be uninstall");
}

bool test_install_with_environment_overrides_calls_configure_environment() {
    auto adapter = std::make_shared<FakeWindowsServiceControlAdapter>();
    redclaw::service::WindowsServiceLifecycleWrapper wrapper(adapter);

    redclaw::service::ServiceInstallConfig config;
    config.service_name = "RedClawHostService";
    config.display_name = "RedClaw Host Service";
    config.binary_path = "C:/Program Files/RedClaw/redclaw_host_service.exe";
    config.environment_overrides = {
        {"REDCLAW_TRUST_STORE_PATH", "C:/redclaw/trusted.txt"},
        {"REDCLAW_TRUST_STORE_FALLBACK", "retain"},
    };

    const auto install_result = wrapper.install(config);
    if (!expect_true(install_result == redclaw::service::ServiceLifecycleError::kNone, "install with env overrides should succeed")) {
        return false;
    }

    bool saw_configure = false;
    bool saw_path = false;
    bool saw_fallback = false;
    for (const auto& op : adapter->operations) {
        saw_configure = saw_configure || (op == "configure_environment:RedClawHostService");
        saw_path = saw_path || (op == "env:REDCLAW_TRUST_STORE_PATH=C:/redclaw/trusted.txt");
        saw_fallback = saw_fallback || (op == "env:REDCLAW_TRUST_STORE_FALLBACK=retain");
    }

    return expect_true(saw_configure, "configure_environment should be called on install")
        && expect_true(saw_path, "trust store path override should be forwarded")
        && expect_true(saw_fallback, "trust store fallback override should be forwarded");
}

bool test_environment_override_failure_propagates_error() {
    auto adapter = std::make_shared<FakeWindowsServiceControlAdapter>();
    adapter->fail_operation = "configure_environment";
    redclaw::service::WindowsServiceLifecycleWrapper wrapper(adapter);

    redclaw::service::ServiceInstallConfig config;
    config.service_name = "RedClawHostService";
    config.display_name = "RedClaw Host Service";
    config.binary_path = "C:/Program Files/RedClaw/redclaw_host_service.exe";
    config.environment_overrides = {
        {"REDCLAW_TRUST_STORE_PATH", "C:/redclaw/trusted.txt"},
    };

    const auto install_result = wrapper.install(config);
    return expect_true(
               install_result == redclaw::service::ServiceLifecycleError::kCommandFailed,
               "configure_environment failure should map to command failed")
        && expect_true(
            wrapper.last_error_detail().find("forced_failure:configure_environment:RedClawHostService") == 0,
            "configure_environment failure detail should propagate");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_invalid_config_fails_fast() && ok;
    ok = test_command_failure_propagates_error_detail() && ok;
    ok = test_install_start_stop_uninstall_emits_expected_operations() && ok;
    ok = test_install_with_environment_overrides_calls_configure_environment() && ok;
    ok = test_environment_override_failure_propagates_error() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_service_windows_service_lifecycle_wrapper_tests" << '\n';
    return 0;
}
