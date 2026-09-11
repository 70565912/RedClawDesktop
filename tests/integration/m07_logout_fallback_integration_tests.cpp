#include <iostream>
#include <string>

#include "redclaw/service/capability_registry.h"
#include "redclaw/service/capability_sync.h"
#include "redclaw/service/helper_launcher.h"
#include "redclaw/service/ipc_channel.h"
#include "redclaw/service/session_detection.h"
#include "redclaw/session/session_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
	if (condition) {
		return true;
	}

	std::cerr << "[FAIL] " << message << '\n';
	return false;
}

redclaw::service::HelperLaunchRequest make_request(std::string session_id, std::uint32_t user_session_id) {
	redclaw::service::HelperLaunchRequest request;
	request.session_id = std::move(session_id);
	request.user_session_id = user_session_id;
	request.helper_executable_path = "C:\\Program Files\\RedClaw\\redclaw_user_session_helper.exe";
	request.service_pipe_name = "\\\\.\\pipe\\redclaw_service_ipc";
	return request;
}

bool test_logout_drives_prelogin_fallback() {
	using redclaw::service::CapabilitySyncState;
	using redclaw::service::IpcErrorCode;
	using redclaw::service::SessionMode;
	using redclaw::session::HandoffState;
	using redclaw::session::HandoffTransitionEvent;

	redclaw::service::SessionDetectionListener detection;
	redclaw::service::HelperProcessLauncher launcher;
	redclaw::session::SessionHandoffStateMachine handoff(HandoffState::kPreLogin);

	redclaw::service::IpcChannelConfig ipc_config;
	ipc_config.pipe_name = "\\\\.\\pipe\\redclaw_logout_fallback";
	redclaw::service::WindowsNamedPipeIpcChannel channel(ipc_config);
	redclaw::service::IpcCapabilitySyncCoordinator capability_sync(channel, SessionMode::kPreLogin);

	if (!expect_true(handoff.on_event(HandoffTransitionEvent::kInteractiveSessionDetected).transitioned, "pre-login -> launching")) {
		return false;
	}
	if (!expect_true(handoff.on_event(HandoffTransitionEvent::kHelperLaunchSucceeded).transitioned, "launching -> sync")) {
		return false;
	}
	if (!expect_true(handoff.on_event(HandoffTransitionEvent::kCapabilitySyncSucceeded).transitioned, "sync -> active")) {
		return false;
	}

	const auto launched = launcher.launch_helper(make_request("m07-logout-fallback", 404));
	if (!expect_true(launched.launched, "initial helper launch should succeed")) {
		return false;
	}

	detection.set_session_change_handler([&](const redclaw::service::SessionChangeEvent& event) {
		if (event.type != redclaw::service::SessionEventType::kLogoff) {
			return;
		}

		(void)handoff.on_event(HandoffTransitionEvent::kUserLoggedOut);
		std::string stop_error;
		(void)launcher.stop_helper("m07-logout-fallback", &stop_error);

		if (channel.connect_to_server()) {
			const auto prelogin_caps = redclaw::service::build_pre_login_capability_set();
			(void)capability_sync.advertise_capabilities(prelogin_caps);
			const auto prelogin_ack =
				redclaw::service::build_capability_acknowledgment(prelogin_caps, true, IpcErrorCode::kNone, 10);
			(void)capability_sync.handle_capability_acknowledgment(prelogin_ack);
		}

		(void)handoff.on_event(HandoffTransitionEvent::kDowngradeCompleted);
	});

	if (!expect_true(detection.start() == redclaw::service::SessionDetectionError::kNone, "session detection starts")) {
		return false;
	}

	detection.handle_session_change(0x6, 404);  // WTS_SESSION_LOGOFF

	const bool state_ok = expect_true(handoff.state() == HandoffState::kPreLogin, "handoff should fallback to pre-login");
	const bool helper_ok = expect_true(!launcher.is_helper_running("m07-logout-fallback"), "helper should be stopped");
	const bool caps_ok = expect_true(
		capability_sync.state() == CapabilitySyncState::kSynchronized
			&& capability_sync.current_capabilities().session_mode == SessionMode::kPreLogin,
		"capability sync should end in pre-login mode");
	const bool stop_ok = expect_true(
		detection.stop() == redclaw::service::SessionDetectionError::kNone,
		"session detection stops cleanly");

	return state_ok && helper_ok && caps_ok && stop_ok;
}

}  // namespace

int main() {
	if (!test_logout_drives_prelogin_fallback()) {
		return 1;
	}

	std::cout << "[PASS] redclaw_m07_logout_fallback_integration_tests" << '\n';
	return 0;
}

