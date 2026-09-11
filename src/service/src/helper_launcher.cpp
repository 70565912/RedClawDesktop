#include "redclaw/service/helper_launcher.h"

#include <chrono>

namespace redclaw::service {

namespace {
std::shared_ptr<IHelperProcessLauncherAdapter> create_default_adapter() {
	return std::make_shared<InMemoryHelperProcessLauncherAdapter>();
}
}  // namespace

bool InMemoryHelperProcessLauncherAdapter::acquire_user_token(
	std::uint32_t user_session_id,
	std::string* token_handle,
	std::string* error_detail) {
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

bool InMemoryHelperProcessLauncherAdapter::launch_process_as_user(
	const HelperLaunchRequest& request,
	std::string_view token_handle,
	std::string_view auth_token,
	std::uint32_t* process_id,
	std::string* error_detail) {
	if (request.helper_executable_path.empty()) {
		if (error_detail) {
			*error_detail = "helper executable path is empty";
		}
		return false;
	}

	if (token_handle.empty() || auth_token.empty()) {
		if (error_detail) {
			*error_detail = "missing token or auth token";
		}
		return false;
	}

	const auto pid = next_process_id_++;
	running_[pid] = true;
	if (process_id) {
		*process_id = pid;
	}
	return true;
}

bool InMemoryHelperProcessLauncherAdapter::terminate_process(
	std::uint32_t process_id,
	std::string* error_detail) {
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

bool InMemoryHelperProcessLauncherAdapter::is_process_running(std::uint32_t process_id) const {
	const auto it = running_.find(process_id);
	return it != running_.end() && it->second;
}

HelperProcessLauncher::HelperProcessLauncher(std::shared_ptr<IHelperProcessLauncherAdapter> adapter)
	: adapter_(adapter ? std::move(adapter) : create_default_adapter()) {}

HelperLaunchResult HelperProcessLauncher::launch_helper(const HelperLaunchRequest& request) {
	HelperLaunchResult result;

	std::string validation_error;
	if (!is_valid_request(request, &validation_error)) {
		result.error = HelperLaunchError::kInvalidRequest;
		result.error_detail = validation_error;
		return result;
	}

	if (active_helpers_.contains(request.session_id) && is_helper_running(request.session_id)) {
		result.error = HelperLaunchError::kAlreadyRunning;
		result.error_detail = "helper already running for session";
		return result;
	}

	std::string token_handle;
	if (!adapter_->acquire_user_token(request.user_session_id, &token_handle, &result.error_detail)) {
		result.error = HelperLaunchError::kTokenAcquisitionFailed;
		return result;
	}

	result.auth_token = generate_auth_token(request.session_id);

	std::uint32_t process_id = 0;
	if (!adapter_->launch_process_as_user(
		request,
		token_handle,
		result.auth_token,
		&process_id,
		&result.error_detail)) {
		result.error = HelperLaunchError::kLaunchFailed;
		return result;
	}

	result.launched = true;
	result.error = HelperLaunchError::kNone;
	result.process_id = process_id;
	result.helper_process_id = "helper-proc-" + std::to_string(process_id);
	active_helpers_[request.session_id] = ActiveHelperEntry {
		.process_id = process_id,
		.auth_token = result.auth_token,
	};

	return result;
}

bool HelperProcessLauncher::stop_helper(std::string_view session_id, std::string* error_detail) {
	const auto key = std::string(session_id);
	const auto it = active_helpers_.find(key);
	if (it == active_helpers_.end()) {
		if (error_detail) {
			*error_detail = "no helper for session";
		}
		return false;
	}

	std::string stop_error;
	if (!adapter_->terminate_process(it->second.process_id, &stop_error)) {
		if (error_detail) {
			*error_detail = stop_error;
		}
		return false;
	}

	active_helpers_.erase(it);
	return true;
}

bool HelperProcessLauncher::is_helper_running(std::string_view session_id) const {
	const auto it = active_helpers_.find(std::string(session_id));
	if (it == active_helpers_.end()) {
		return false;
	}

	return adapter_->is_process_running(it->second.process_id);
}

std::optional<std::uint32_t> HelperProcessLauncher::helper_process_id(std::string_view session_id) const {
	const auto it = active_helpers_.find(std::string(session_id));
	if (it == active_helpers_.end()) {
		return std::nullopt;
	}

	return it->second.process_id;
}

bool HelperProcessLauncher::is_valid_request(const HelperLaunchRequest& request, std::string* error_detail) {
	if (request.session_id.empty()) {
		if (error_detail) {
			*error_detail = "session id is required";
		}
		return false;
	}

	if (request.user_session_id == 0) {
		if (error_detail) {
			*error_detail = "user session id is required";
		}
		return false;
	}

	if (request.helper_executable_path.empty()) {
		if (error_detail) {
			*error_detail = "helper executable path is required";
		}
		return false;
	}

	return true;
}

std::string HelperProcessLauncher::generate_auth_token(const std::string& session_id) {
	const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	++token_counter_;
	return "auth-" + session_id + "-" + std::to_string(now) + "-" + std::to_string(token_counter_);
}

}  // namespace redclaw::service

