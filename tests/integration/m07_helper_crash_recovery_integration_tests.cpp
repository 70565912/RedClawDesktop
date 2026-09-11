#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>

#include "redclaw/service/helper_launcher.h"
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

class FailAfterFirstLaunchAdapter final : public redclaw::service::IHelperProcessLauncherAdapter {
public:
	[[nodiscard]] bool acquire_user_token(
		std::uint32_t user_session_id,
		std::string* token_handle,
		std::string* error_detail) override {
		if (user_session_id == 0) {
			if (error_detail) {
				*error_detail = "invalid session id";
			}
			return false;
		}
		if (token_handle) {
			*token_handle = "token-session-" + std::to_string(user_session_id);
		}
		return true;
	}

	[[nodiscard]] bool launch_process_as_user(
		const redclaw::service::HelperLaunchRequest& request,
		std::string_view token_handle,
		std::string_view auth_token,
		std::uint32_t* process_id,
		std::string* error_detail) override {
		if (request.helper_executable_path.empty() || token_handle.empty() || auth_token.empty()) {
			if (error_detail) {
				*error_detail = "invalid launch inputs";
			}
			return false;
		}

		++launch_attempts_;
		if (launch_attempts_ > 1) {
			if (error_detail) {
				*error_detail = "simulated launch failure";
			}
			return false;
		}

		const auto pid = next_pid_++;
		running_[pid] = true;
		if (process_id) {
			*process_id = pid;
		}
		return true;
	}

	[[nodiscard]] bool terminate_process(
		std::uint32_t process_id,
		std::string* error_detail) override {
		const auto it = running_.find(process_id);
		if (it == running_.end() || !it->second) {
			if (error_detail) {
				*error_detail = "process not running";
			}
			return false;
		}
		it->second = false;
		return true;
	}

	[[nodiscard]] bool is_process_running(std::uint32_t process_id) const override {
		const auto it = running_.find(process_id);
		return it != running_.end() && it->second;
	}

private:
	std::unordered_map<std::uint32_t, bool> running_;
	std::uint32_t next_pid_ = 7000;
	std::uint32_t launch_attempts_ = 0;
};

bool test_helper_crash_bounded_restart_succeeds() {
	using redclaw::session::HandoffState;
	using redclaw::session::HandoffTransitionEvent;

	auto adapter = std::make_shared<redclaw::service::InMemoryHelperProcessLauncherAdapter>();
	redclaw::service::HelperProcessLauncher launcher(adapter);
	redclaw::session::SessionHandoffStateMachine handoff(HandoffState::kUserSessionActive);

	const auto initial = launcher.launch_helper(make_request("m07-crash-restart-success", 201));
	if (!expect_true(initial.launched, "initial helper launch should succeed")) {
		return false;
	}

	std::string terminate_error;
	if (!expect_true(adapter->terminate_process(initial.process_id, &terminate_error), "simulate helper crash by terminating process")) {
		return false;
	}
	if (!expect_true(!launcher.is_helper_running("m07-crash-restart-success"), "launcher should observe helper is no longer running")) {
		return false;
	}

	if (!expect_true(handoff.on_event(HandoffTransitionEvent::kHelperLost).transitioned, "active -> downgrading on helper loss")) {
		return false;
	}

	const std::uint32_t max_restart_attempts = 3;
	bool restarted = false;
	for (std::uint32_t attempt = 0; attempt < max_restart_attempts; ++attempt) {
		const auto restart = launcher.launch_helper(make_request("m07-crash-restart-success", 201));
		if (restart.launched) {
			restarted = true;
			(void)handoff.on_event(HandoffTransitionEvent::kDowngradeCompleted);
			(void)handoff.on_event(HandoffTransitionEvent::kInteractiveSessionDetected);
			(void)handoff.on_event(HandoffTransitionEvent::kHelperLaunchSucceeded);
			(void)handoff.on_event(HandoffTransitionEvent::kCapabilitySyncSucceeded);
			break;
		}
	}

	const bool restart_ok = expect_true(restarted, "bounded restart should recover helper");
	const bool state_ok = expect_true(handoff.state() == HandoffState::kUserSessionActive, "handoff should return to active after restart");
	const bool running_ok = expect_true(launcher.is_helper_running("m07-crash-restart-success"), "replacement helper should be running");

	std::string stop_error;
	const bool cleanup_ok = expect_true(launcher.stop_helper("m07-crash-restart-success", &stop_error), "cleanup stop should succeed");

	return restart_ok && state_ok && running_ok && cleanup_ok;
}

bool test_helper_crash_bounded_restart_falls_back() {
	using redclaw::session::HandoffState;
	using redclaw::session::HandoffTransitionEvent;

	auto adapter = std::make_shared<FailAfterFirstLaunchAdapter>();
	redclaw::service::HelperProcessLauncher launcher(adapter);
	redclaw::session::SessionHandoffStateMachine handoff(HandoffState::kUserSessionActive);

	const auto initial = launcher.launch_helper(make_request("m07-crash-restart-fallback", 301));
	if (!expect_true(initial.launched, "initial helper launch should succeed")) {
		return false;
	}

	std::string terminate_error;
	if (!expect_true(adapter->terminate_process(initial.process_id, &terminate_error), "simulate helper crash by terminating process")) {
		return false;
	}

	if (!expect_true(handoff.on_event(HandoffTransitionEvent::kHelperLost).transitioned, "active -> downgrading on helper loss")) {
		return false;
	}

	const std::uint32_t max_restart_attempts = 2;
	bool restarted = false;
	for (std::uint32_t attempt = 0; attempt < max_restart_attempts; ++attempt) {
		const auto restart = launcher.launch_helper(make_request("m07-crash-restart-fallback", 301));
		if (restart.launched) {
			restarted = true;
			break;
		}
	}

	if (!expect_true(!restarted, "restart should fail within bounded attempts")) {
		return false;
	}

	const bool fallback_ok = expect_true(
		handoff.on_event(HandoffTransitionEvent::kDowngradeCompleted).transitioned
			&& handoff.state() == HandoffState::kPreLogin,
		"handoff should fallback to pre-login when restart budget exhausted");

	const bool running_ok = expect_true(
		!launcher.is_helper_running("m07-crash-restart-fallback"),
		"helper should remain stopped after fallback");

	return fallback_ok && running_ok;
}

}  // namespace

int main() {
	bool ok = true;
	ok = test_helper_crash_bounded_restart_succeeds() && ok;
	ok = test_helper_crash_bounded_restart_falls_back() && ok;

	if (!ok) {
		return 1;
	}

	std::cout << "[PASS] redclaw_m07_helper_crash_recovery_integration_tests" << '\n';
	return 0;
}

