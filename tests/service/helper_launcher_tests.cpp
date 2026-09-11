#include "redclaw/service/helper_launcher.h"

#include <cassert>
#include <iostream>

using namespace redclaw::service;

static HelperLaunchRequest base_request() {
	HelperLaunchRequest req;
	req.session_id = "session-1";
	req.user_session_id = 2;
	req.helper_executable_path = "C:\\Program Files\\RedClaw\\redclaw_user_session_helper.exe";
	req.service_pipe_name = "\\\\.\\pipe\\redclaw_service_ipc";
	return req;
}

static void test_launch_success() {
	HelperProcessLauncher launcher;
	const auto result = launcher.launch_helper(base_request());

	assert(result.launched);
	assert(result.error == HelperLaunchError::kNone);
	assert(result.process_id != 0);
	assert(!result.auth_token.empty());
	assert(launcher.is_helper_running("session-1"));

	std::cout << "✓ Launch success\n";
}

static void test_reject_duplicate_launch() {
	HelperProcessLauncher launcher;
	const auto first = launcher.launch_helper(base_request());
	assert(first.launched);

	const auto second = launcher.launch_helper(base_request());
	assert(!second.launched);
	assert(second.error == HelperLaunchError::kAlreadyRunning);

	std::cout << "✓ Duplicate launch rejection\n";
}

static void test_invalid_request() {
	HelperProcessLauncher launcher;
	auto req = base_request();
	req.helper_executable_path.clear();

	const auto result = launcher.launch_helper(req);
	assert(!result.launched);
	assert(result.error == HelperLaunchError::kInvalidRequest);
	assert(!result.error_detail.empty());

	std::cout << "✓ Invalid request handling\n";
}

static void test_stop_helper() {
	HelperProcessLauncher launcher;
	const auto result = launcher.launch_helper(base_request());
	assert(result.launched);

	std::string error;
	const auto stopped = launcher.stop_helper("session-1", &error);
	assert(stopped);
	assert(!launcher.is_helper_running("session-1"));

	std::cout << "✓ Stop helper\n";
}

static void test_stop_missing_helper() {
	HelperProcessLauncher launcher;

	std::string error;
	const auto stopped = launcher.stop_helper("missing-session", &error);
	assert(!stopped);
	assert(!error.empty());

	std::cout << "✓ Stop missing helper\n";
}

static void test_helper_process_lookup() {
	HelperProcessLauncher launcher;
	const auto result = launcher.launch_helper(base_request());
	assert(result.launched);

	const auto pid = launcher.helper_process_id("session-1");
	assert(pid.has_value());
	assert(*pid == result.process_id);

	const auto missing = launcher.helper_process_id("missing");
	assert(!missing.has_value());

	std::cout << "✓ Helper process lookup\n";
}

int main() {
	std::cout << "Running helper launcher tests...\n";
	test_launch_success();
	test_reject_duplicate_launch();
	test_invalid_request();
	test_stop_helper();
	test_stop_missing_helper();
	test_helper_process_lookup();
	std::cout << "All helper launcher tests passed!\n";
	return 0;
}

