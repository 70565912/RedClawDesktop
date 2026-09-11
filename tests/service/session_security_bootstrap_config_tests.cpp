#include <iostream>
#include <string>

#include "redclaw/service/service_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

#if defined(_WIN32)
bool set_env(const char* key, const char* value) {
    return _putenv_s(key, value) == 0;
}

void unset_env(const char* key) {
    (void)_putenv_s(key, "");
}
#else
bool set_env(const char* key, const char* value) {
    return setenv(key, value, 1) == 0;
}

void unset_env(const char* key) {
    (void)unsetenv(key);
}
#endif

bool test_defaults_when_env_missing() {
    unset_env("REDCLAW_TRUST_STORE_PATH");
    unset_env("REDCLAW_TRUST_STORE_FALLBACK");

    const auto config = redclaw::service::resolve_session_security_bootstrap_config();
    return expect_true(config.trust_store_path == "trusted-peer-fingerprints.txt", "default trust_store_path mismatch")
        && expect_true(config.trust_store_fallback == "clear", "default trust_store_fallback mismatch");
}

bool test_env_overrides_take_effect() {
    if (!expect_true(set_env("REDCLAW_TRUST_STORE_PATH", "C:/redclaw/custom-fp.txt"), "set trust path env failed")) {
        return false;
    }
    if (!expect_true(set_env("REDCLAW_TRUST_STORE_FALLBACK", "retain"), "set fallback env failed")) {
        return false;
    }

    const auto config = redclaw::service::resolve_session_security_bootstrap_config();

    unset_env("REDCLAW_TRUST_STORE_PATH");
    unset_env("REDCLAW_TRUST_STORE_FALLBACK");

    return expect_true(config.trust_store_path == "C:/redclaw/custom-fp.txt", "env trust_store_path mismatch")
        && expect_true(config.trust_store_fallback == "retain", "env trust_store_fallback mismatch");
}

bool test_invalid_fallback_normalizes_to_clear() {
    if (!expect_true(set_env("REDCLAW_TRUST_STORE_FALLBACK", "unknown-policy"), "set fallback env failed")) {
        return false;
    }

    const auto config = redclaw::service::resolve_session_security_bootstrap_config();
    unset_env("REDCLAW_TRUST_STORE_FALLBACK");

    return expect_true(config.trust_store_fallback == "clear", "invalid fallback should normalize to clear");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_defaults_when_env_missing() && ok;
    ok = test_env_overrides_take_effect() && ok;
    ok = test_invalid_fallback_normalizes_to_clear() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_service_session_security_bootstrap_config_tests" << '\n';
    return 0;
}
