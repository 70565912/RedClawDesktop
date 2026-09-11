#include <iostream>
#include <string>

#include "redclaw/service/capability_registry.h"

namespace {

using namespace redclaw::service;

void expect_true(bool condition, const std::string& message) {
	if (!condition) {
		std::cerr << "FAIL: " << message << "\n";
		std::exit(1);
	}
}

void test_pre_login_capability_set() {
	const CapabilitySet caps = build_pre_login_capability_set();
	
	expect_true(caps.session_mode == SessionMode::kPreLogin, "session mode is pre-login");
	expect_true(caps.secure_desktop_capture, "has secure desktop capture");
	expect_true(!caps.secure_desktop_input, "no secure desktop input by default");
	expect_true(!caps.user_desktop_capture, "no user desktop capture");
	expect_true(!caps.user_desktop_input, "no user desktop input");
	expect_true(!caps.clipboard_sync, "no clipboard sync");
	expect_true(!caps.file_transfer, "no file transfer");
	expect_true(caps.helper_capabilities == HelperCapabilityFlag::kNone, "no helper capabilities");
}

void test_user_session_capability_set() {
	const CapabilitySet caps = build_user_session_capability_set();
	
	expect_true(caps.session_mode == SessionMode::kUserSession, "session mode is user session");
	expect_true(caps.user_desktop_capture, "has user desktop capture");
	expect_true(caps.user_desktop_input, "has user desktop input");
	expect_true(caps.clipboard_sync, "has clipboard sync");
	expect_true(caps.file_transfer, "has file transfer");
	expect_true(caps.notifications, "has notifications");
	expect_true(caps.audio_capture, "has audio capture");
	
	expect_true(has_capability(caps.helper_capabilities, HelperCapabilityFlag::kUserDesktopCapture),
	            "helper capabilities include desktop capture");
	expect_true(has_capability(caps.helper_capabilities, HelperCapabilityFlag::kClipboardSync),
	            "helper capabilities include clipboard");
}

void test_secure_desktop_capability_set() {
	const CapabilitySet caps = build_secure_desktop_capability_set();
	
	expect_true(caps.secure_desktop_capture, "has secure desktop capture");
	expect_true(!caps.secure_desktop_input, "no secure desktop input by default");
	expect_true(!caps.user_desktop_capture, "no user desktop features");
	expect_true(!caps.clipboard_sync, "no clipboard");
	expect_true(caps.helper_capabilities == HelperCapabilityFlag::kNone, "no helper capabilities");
}

void test_capability_upgrade_detection() {
	const CapabilitySet pre_login = build_pre_login_capability_set();
	const CapabilitySet user_session = build_user_session_capability_set();
	
	expect_true(is_capability_upgrade(pre_login, user_session), "pre-login to user-session is upgrade");
	expect_true(!is_capability_downgrade(pre_login, user_session), "not a downgrade");
}

void test_capability_downgrade_detection() {
	const CapabilitySet pre_login = build_pre_login_capability_set();
	const CapabilitySet user_session = build_user_session_capability_set();
	
	expect_true(is_capability_downgrade(user_session, pre_login), "user-session to pre-login is downgrade");
	expect_true(!is_capability_upgrade(user_session, pre_login), "not an upgrade");
}

void test_valid_capability_transitions() {
	const CapabilitySet pre_login = build_pre_login_capability_set();
	const CapabilitySet user_session = build_user_session_capability_set();
	const CapabilitySet secure_desktop = build_secure_desktop_capability_set();
	
	// Upgrade transitions
	expect_true(is_valid_capability_transition(pre_login, user_session),
	            "pre-login to user-session is valid");
	
	// Downgrade transitions (always valid for security)
	expect_true(is_valid_capability_transition(user_session, pre_login),
	            "user-session to pre-login is valid (downgrade)");
	expect_true(is_valid_capability_transition(user_session, secure_desktop),
	            "user-session to secure-desktop is valid (downgrade)");
	
	// Same-mode transitions
	expect_true(is_valid_capability_transition(pre_login, pre_login),
	            "same mode transition is valid");
}

void test_advertised_capabilities() {
	const auto pre_login_caps = get_advertised_capabilities(SessionMode::kPreLogin);
	expect_true(pre_login_caps == HelperCapabilityFlag::kNone,
	            "pre-login mode advertises no helper capabilities");
	
	const auto user_session_caps = get_advertised_capabilities(SessionMode::kUserSession);
	expect_true(has_capability(user_session_caps, HelperCapabilityFlag::kUserDesktopCapture),
	            "user-session advertises desktop capture");
	expect_true(has_capability(user_session_caps, HelperCapabilityFlag::kClipboardSync),
	            "user-session advertises clipboard");
}

void test_capability_helpers() {
	const CapabilitySet user_session = build_user_session_capability_set();
	
	expect_true(has_desktop_capture(user_session), "has desktop capture");
	expect_true(has_desktop_input(user_session), "has desktop input");
	expect_true(has_clipboard(user_session), "has clipboard");
	expect_true(has_file_transfer(user_session), "has file transfer");
	
	const CapabilitySet pre_login = build_pre_login_capability_set();
	expect_true(has_desktop_capture(pre_login), "pre-login has secure desktop capture");
	expect_true(!has_clipboard(pre_login), "pre-login has no clipboard");
}

void test_capability_diff() {
	const CapabilitySet pre_login = build_pre_login_capability_set();
	const CapabilitySet user_session = build_user_session_capability_set();
	
	const auto diff = get_capability_diff(pre_login, user_session);
	
	expect_true(!diff.empty(), "diff is not empty");
	expect_true(diff.size() >= 5, "multiple capabilities changed");
	
	// Check that key changes are reported
	bool found_session_mode = false;
	bool found_user_desktop = false;
	bool found_clipboard = false;
	
	for (const auto& change : diff) {
		if (change.find("session_mode") != std::string::npos) found_session_mode = true;
		if (change.find("user_desktop_capture") != std::string::npos) found_user_desktop = true;
		if (change.find("clipboard_sync") != std::string::npos) found_clipboard = true;
	}
	
	expect_true(found_session_mode, "diff includes session mode change");
	expect_true(found_user_desktop, "diff includes user desktop change");
	expect_true(found_clipboard, "diff includes clipboard change");
}

void test_describe_capability_set() {
	const CapabilitySet user_session = build_user_session_capability_set();
	const std::string description = describe_capability_set(user_session);
	
	expect_true(!description.empty(), "description is not empty");
	expect_true(description.find("UserSession") != std::string::npos, "includes session mode");
	expect_true(description.find("user_desktop_capture") != std::string::npos, "includes features");
}

void test_to_string_helpers() {
	expect_true(to_string(SessionMode::kPreLogin) == "PreLogin", "session mode to_string");
	expect_true(to_string(SessionMode::kUserSession) == "UserSession", "session mode to_string");
	
	expect_true(to_string(CapabilityUpgradeReason::kUserLogin) == "UserLogin",
	            "upgrade reason to_string");
	expect_true(to_string(CapabilityDowngradeReason::kUserLogout) == "UserLogout",
	            "downgrade reason to_string");
}

void test_no_capabilities_same_as_none() {
	const CapabilitySet pre_login = build_pre_login_capability_set();
	expect_true(!is_capability_upgrade(pre_login, pre_login), "same capabilities not an upgrade");
	expect_true(!is_capability_downgrade(pre_login, pre_login), "same capabilities not a downgrade");
}

void test_secure_desktop_downgrade() {
	const CapabilitySet user_session = build_user_session_capability_set();
	const CapabilitySet secure_desktop = build_secure_desktop_capability_set();
	
	expect_true(is_capability_downgrade(user_session, secure_desktop),
	            "user-session to secure-desktop is downgrade");
	expect_true(is_valid_capability_transition(user_session, secure_desktop),
	            "downgrade to secure-desktop is valid");
}

}  // namespace

int main() {
	std::cout << "Running capability registry tests...\n";
	
	test_pre_login_capability_set();
	test_user_session_capability_set();
	test_secure_desktop_capability_set();
	test_capability_upgrade_detection();
	test_capability_downgrade_detection();
	test_valid_capability_transitions();
	test_advertised_capabilities();
	test_capability_helpers();
	test_capability_diff();
	test_describe_capability_set();
	test_to_string_helpers();
	test_no_capabilities_same_as_none();
	test_secure_desktop_downgrade();
	
	std::cout << "All capability registry tests passed!\n";
	return 0;
}
