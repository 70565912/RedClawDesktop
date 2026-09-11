#include "redclaw/service/capability_registry.h"

#include <sstream>

namespace redclaw::service {

CapabilitySet build_pre_login_capability_set() {
	CapabilitySet caps;
	caps.session_mode = SessionMode::kPreLogin;
	
	// Pre-login mode: Only secure desktop capabilities available
	caps.secure_desktop_capture = true;
	caps.secure_desktop_input = false;  // Input requires explicit policy grant
	
	// User desktop features not available
	caps.user_desktop_capture = false;
	caps.user_desktop_input = false;
	caps.clipboard_sync = false;
	caps.file_transfer = false;
	caps.notifications = false;
	caps.audio_capture = false;
	
	caps.helper_capabilities = HelperCapabilityFlag::kNone;
	
	return caps;
}

CapabilitySet build_user_session_capability_set() {
	CapabilitySet caps;
	caps.session_mode = SessionMode::kUserSession;
	
	// User session mode: Full desktop integration available
	caps.user_desktop_capture = true;
	caps.user_desktop_input = true;
	caps.clipboard_sync = true;
	caps.file_transfer = true;
	caps.notifications = true;
	caps.audio_capture = true;
	
	// Secure desktop still available when needed
	caps.secure_desktop_capture = true;
	caps.secure_desktop_input = false;  // Still requires policy grant
	
	// Helper capabilities flags
	caps.helper_capabilities = 
		HelperCapabilityFlag::kUserDesktopCapture |
		HelperCapabilityFlag::kUserInputInjection |
		HelperCapabilityFlag::kClipboardSync |
		HelperCapabilityFlag::kFileTransfer |
		HelperCapabilityFlag::kNotifications;
	
	return caps;
}

CapabilitySet build_secure_desktop_capability_set() {
	CapabilitySet caps;
	caps.session_mode = SessionMode::kPreLogin;  // Secure desktop is similar to pre-login
	
	// Secure desktop active: Only secure desktop capture, no input by default
	caps.secure_desktop_capture = true;
	caps.secure_desktop_input = false;  // Requires explicit full-control policy
	
	// All user desktop features disabled
	caps.user_desktop_capture = false;
	caps.user_desktop_input = false;
	caps.clipboard_sync = false;
	caps.file_transfer = false;
	caps.notifications = false;
	caps.audio_capture = false;
	
	caps.helper_capabilities = HelperCapabilityFlag::kNone;
	
	return caps;
}

bool is_valid_capability_transition(const CapabilitySet& from, const CapabilitySet& to) {
	// Allow all downgrades (security-safe)
	if (is_capability_downgrade(from, to)) {
		return true;
	}
	
	// Allow upgrades based on session mode changes
	if (from.session_mode == SessionMode::kPreLogin && 
	    to.session_mode == SessionMode::kUserSession) {
		return true;
	}
	
	// Allow same-mode transitions (policy changes)
	if (from.session_mode == to.session_mode) {
		return true;
	}
	
	// Disallow downgrade in session mode (user → pre-login without explicit reason)
	if (from.session_mode == SessionMode::kUserSession && 
	    to.session_mode == SessionMode::kPreLogin) {
		// This should only happen through explicit downgrade, which is already allowed above
		return false;
	}
	
	return true;
}

HelperCapabilityFlag get_advertised_capabilities(SessionMode mode) {
	switch (mode) {
		case SessionMode::kPreLogin:
			return HelperCapabilityFlag::kNone;
		
		case SessionMode::kUserSession:
			return HelperCapabilityFlag::kUserDesktopCapture |
			       HelperCapabilityFlag::kUserInputInjection |
			       HelperCapabilityFlag::kClipboardSync |
			       HelperCapabilityFlag::kFileTransfer |
			       HelperCapabilityFlag::kNotifications;
		
		default:
			return HelperCapabilityFlag::kNone;
	}
}

bool has_desktop_capture(const CapabilitySet& caps) {
	return caps.user_desktop_capture || caps.secure_desktop_capture;
}

bool has_desktop_input(const CapabilitySet& caps) {
	return caps.user_desktop_input || caps.secure_desktop_input;
}

bool has_clipboard(const CapabilitySet& caps) {
	return caps.clipboard_sync;
}

bool has_file_transfer(const CapabilitySet& caps) {
	return caps.file_transfer;
}

bool is_capability_upgrade(const CapabilitySet& from, const CapabilitySet& to) {
	int from_count = 0;
	int to_count = 0;
	
	if (from.user_desktop_capture) from_count++;
	if (from.user_desktop_input) from_count++;
	if (from.clipboard_sync) from_count++;
	if (from.file_transfer) from_count++;
	if (from.notifications) from_count++;
	if (from.audio_capture) from_count++;
	
	if (to.user_desktop_capture) to_count++;
	if (to.user_desktop_input) to_count++;
	if (to.clipboard_sync) to_count++;
	if (to.file_transfer) to_count++;
	if (to.notifications) to_count++;
	if (to.audio_capture) to_count++;
	
	return to_count > from_count;
}

bool is_capability_downgrade(const CapabilitySet& from, const CapabilitySet& to) {
	int from_count = 0;
	int to_count = 0;
	
	if (from.user_desktop_capture) from_count++;
	if (from.user_desktop_input) from_count++;
	if (from.clipboard_sync) from_count++;
	if (from.file_transfer) from_count++;
	if (from.notifications) from_count++;
	if (from.audio_capture) from_count++;
	
	if (to.user_desktop_capture) to_count++;
	if (to.user_desktop_input) to_count++;
	if (to.clipboard_sync) to_count++;
	if (to.file_transfer) to_count++;
	if (to.notifications) to_count++;
	if (to.audio_capture) to_count++;
	
	return to_count < from_count;
}

std::string describe_capability_set(const CapabilitySet& caps) {
	std::ostringstream oss;
	oss << "CapabilitySet{mode=" << to_string(caps.session_mode);
	
	std::vector<std::string> features;
	if (caps.user_desktop_capture) features.push_back("user_desktop_capture");
	if (caps.user_desktop_input) features.push_back("user_desktop_input");
	if (caps.secure_desktop_capture) features.push_back("secure_desktop_capture");
	if (caps.secure_desktop_input) features.push_back("secure_desktop_input");
	if (caps.clipboard_sync) features.push_back("clipboard");
	if (caps.file_transfer) features.push_back("file_transfer");
	if (caps.notifications) features.push_back("notifications");
	if (caps.audio_capture) features.push_back("audio");
	
	if (!features.empty()) {
		oss << ", features=[";
		for (size_t i = 0; i < features.size(); ++i) {
			if (i > 0) oss << ", ";
			oss << features[i];
		}
		oss << "]";
	}
	
	oss << "}";
	return oss.str();
}

std::vector<std::string> get_capability_diff(const CapabilitySet& from, const CapabilitySet& to) {
	std::vector<std::string> diff;
	
	if (from.session_mode != to.session_mode) {
		diff.push_back("session_mode: " + to_string(from.session_mode) + " -> " + to_string(to.session_mode));
	}
	
	if (from.user_desktop_capture != to.user_desktop_capture) {
		diff.push_back(std::string("user_desktop_capture: ") + 
		               (to.user_desktop_capture ? "enabled" : "disabled"));
	}
	
	if (from.user_desktop_input != to.user_desktop_input) {
		diff.push_back(std::string("user_desktop_input: ") + 
		               (to.user_desktop_input ? "enabled" : "disabled"));
	}
	
	if (from.secure_desktop_input != to.secure_desktop_input) {
		diff.push_back(std::string("secure_desktop_input: ") + 
		               (to.secure_desktop_input ? "enabled" : "disabled"));
	}
	
	if (from.clipboard_sync != to.clipboard_sync) {
		diff.push_back(std::string("clipboard_sync: ") + 
		               (to.clipboard_sync ? "enabled" : "disabled"));
	}
	
	if (from.file_transfer != to.file_transfer) {
		diff.push_back(std::string("file_transfer: ") + 
		               (to.file_transfer ? "enabled" : "disabled"));
	}
	
	if (from.notifications != to.notifications) {
		diff.push_back(std::string("notifications: ") + 
		               (to.notifications ? "enabled" : "disabled"));
	}
	
	if (from.audio_capture != to.audio_capture) {
		diff.push_back(std::string("audio_capture: ") + 
		               (to.audio_capture ? "enabled" : "disabled"));
	}
	
	return diff;
}

std::string to_string(SessionMode mode) {
	switch (mode) {
		case SessionMode::kPreLogin: return "PreLogin";
		case SessionMode::kUserSession: return "UserSession";
		default: return "Unknown";
	}
}

std::string to_string(CapabilityUpgradeReason reason) {
	switch (reason) {
		case CapabilityUpgradeReason::kUserLogin: return "UserLogin";
		case CapabilityUpgradeReason::kHelperStarted: return "HelperStarted";
		case CapabilityUpgradeReason::kPolicyChange: return "PolicyChange";
		default: return "Unknown";
	}
}

std::string to_string(CapabilityDowngradeReason reason) {
	switch (reason) {
		case CapabilityDowngradeReason::kUserLogout: return "UserLogout";
		case CapabilityDowngradeReason::kHelperCrashed: return "HelperCrashed";
		case CapabilityDowngradeReason::kHelperStopped: return "HelperStopped";
		case CapabilityDowngradeReason::kSecureDesktopActive: return "SecureDesktopActive";
		case CapabilityDowngradeReason::kPolicyRevoked: return "PolicyRevoked";
		default: return "Unknown";
	}
}

}  // namespace redclaw::service
