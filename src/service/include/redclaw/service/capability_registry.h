#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "redclaw/service/ipc_channel_contracts.h"

namespace redclaw::service {

enum class SessionMode {
	kPreLogin,      // Service-only mode (session 0, no interactive user)
	kUserSession,   // User-session helper active (user desktop context)
};

struct CapabilitySet {
	HelperCapabilityFlag helper_capabilities = HelperCapabilityFlag::kNone;
	bool secure_desktop_capture = false;
	bool secure_desktop_input = false;
	bool user_desktop_capture = false;
	bool user_desktop_input = false;
	bool clipboard_sync = false;
	bool file_transfer = false;
	bool notifications = false;
	bool audio_capture = false;
	SessionMode session_mode = SessionMode::kPreLogin;
};

struct CapabilityTransition {
	CapabilitySet before;
	CapabilitySet after;
	std::string reason;
	std::uint64_t timestamp_ms = 0;
};

enum class CapabilityUpgradeReason {
	kUserLogin,
	kHelperStarted,
	kPolicyChange,
};

enum class CapabilityDowngradeReason {
	kUserLogout,
	kHelperCrashed,
	kHelperStopped,
	kSecureDesktopActive,
	kPolicyRevoked,
};

// Pre-login capability set (session 0 service-only mode)
[[nodiscard]] CapabilitySet build_pre_login_capability_set();

// User-session capability set (helper process active in user desktop)
[[nodiscard]] CapabilitySet build_user_session_capability_set();

// Secure-desktop capability set (UAC prompt active, limited capabilities)
[[nodiscard]] CapabilitySet build_secure_desktop_capability_set();

// Check if a capability transition is valid
[[nodiscard]] bool is_valid_capability_transition(
	const CapabilitySet& from,
	const CapabilitySet& to);

// Determine what capabilities to advertise based on current session mode
[[nodiscard]] HelperCapabilityFlag get_advertised_capabilities(SessionMode mode);

// Check if a specific capability is available in a capability set
[[nodiscard]] bool has_desktop_capture(const CapabilitySet& caps);
[[nodiscard]] bool has_desktop_input(const CapabilitySet& caps);
[[nodiscard]] bool has_clipboard(const CapabilitySet& caps);
[[nodiscard]] bool has_file_transfer(const CapabilitySet& caps);

// Capability set comparison
[[nodiscard]] bool is_capability_upgrade(const CapabilitySet& from, const CapabilitySet& to);
[[nodiscard]] bool is_capability_downgrade(const CapabilitySet& from, const CapabilitySet& to);

// Describe capability set for logging/debugging
[[nodiscard]] std::string describe_capability_set(const CapabilitySet& caps);

// Get capability difference for logging transitions
[[nodiscard]] std::vector<std::string> get_capability_diff(
	const CapabilitySet& from,
	const CapabilitySet& to);

std::string to_string(SessionMode mode);
std::string to_string(CapabilityUpgradeReason reason);
std::string to_string(CapabilityDowngradeReason reason);

}  // namespace redclaw::service
