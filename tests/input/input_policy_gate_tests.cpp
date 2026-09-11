#include <iostream>
#include <string>

#include "redclaw/input/input_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool test_deny_all_policy() {
    redclaw::input::InputPolicyGate gate;
    gate.set_permission_policy(redclaw::input::InputPermissionPolicy::kDenyAll);

    return expect_true(!gate.can_inject(redclaw::input::InputTarget::kUserDesktop), "deny-all should block user desktop")
        && expect_true(
            !gate.can_inject(redclaw::input::InputTarget::kSecureDesktop),
            "deny-all should block secure desktop")
        && expect_true(
            gate.deny_reason(redclaw::input::InputTarget::kUserDesktop) == "policy_denies_all_input",
            "deny-all reason should match");
}

bool test_standard_desktop_only_policy() {
    redclaw::input::InputPolicyGate gate;
    gate.set_permission_policy(redclaw::input::InputPermissionPolicy::kStandardDesktopOnly);

    return expect_true(gate.can_inject(redclaw::input::InputTarget::kUserDesktop), "standard policy should allow user desktop")
        && expect_true(
            !gate.can_inject(redclaw::input::InputTarget::kSecureDesktop),
            "standard policy should block secure desktop");
}

bool test_secure_desktop_requires_privileged_mode() {
    redclaw::input::InputPolicyGate gate;
    gate.set_permission_policy(redclaw::input::InputPermissionPolicy::kFullControlSecureDesktop);
    gate.set_privileged_control_mode(redclaw::input::PrivilegedControlMode::kDisabled);

    const bool blocked_without_privileged =
        !gate.can_inject(redclaw::input::InputTarget::kSecureDesktop)
        && gate.deny_reason(redclaw::input::InputTarget::kSecureDesktop) == "secure_desktop_requires_privileged_mode";

    gate.set_privileged_control_mode(redclaw::input::PrivilegedControlMode::kEnabled);

    const bool allowed_with_privileged = gate.can_inject(redclaw::input::InputTarget::kSecureDesktop);

    return expect_true(blocked_without_privileged, "secure desktop should be blocked without privileged mode")
        && expect_true(allowed_with_privileged, "secure desktop should be allowed with privileged mode");
}

}  // namespace

int main() {
    bool ok = true;
    ok = test_deny_all_policy() && ok;
    ok = test_standard_desktop_only_policy() && ok;
    ok = test_secure_desktop_requires_privileged_mode() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "[PASS] redclaw_input_policy_gate_tests" << '\n';
    return 0;
}
