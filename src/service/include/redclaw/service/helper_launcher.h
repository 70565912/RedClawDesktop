#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace redclaw::service {

enum class HelperLaunchError {
	kNone,
	kInvalidRequest,
	kAlreadyRunning,
	kTokenAcquisitionFailed,
	kLaunchFailed,
	kStopFailed,
};

struct HelperLaunchRequest {
	std::string session_id;
	std::uint32_t user_session_id = 0;
	std::string helper_executable_path;
	std::string service_pipe_name = "\\\\.\\pipe\\redclaw_service_ipc";
};

struct HelperLaunchResult {
	bool launched = false;
	HelperLaunchError error = HelperLaunchError::kNone;
	std::uint32_t process_id = 0;
	std::string auth_token;
	std::string helper_process_id;
	std::string error_detail;
};

class IHelperProcessLauncherAdapter {
public:
	virtual ~IHelperProcessLauncherAdapter() = default;

	[[nodiscard]] virtual bool acquire_user_token(
		std::uint32_t user_session_id,
		std::string* token_handle,
		std::string* error_detail) = 0;

	[[nodiscard]] virtual bool launch_process_as_user(
		const HelperLaunchRequest& request,
		std::string_view token_handle,
		std::string_view auth_token,
		std::uint32_t* process_id,
		std::string* error_detail) = 0;

	[[nodiscard]] virtual bool terminate_process(
		std::uint32_t process_id,
		std::string* error_detail) = 0;

	[[nodiscard]] virtual bool is_process_running(std::uint32_t process_id) const = 0;
};

class InMemoryHelperProcessLauncherAdapter final : public IHelperProcessLauncherAdapter {
public:
	[[nodiscard]] bool acquire_user_token(
		std::uint32_t user_session_id,
		std::string* token_handle,
		std::string* error_detail) override;

	[[nodiscard]] bool launch_process_as_user(
		const HelperLaunchRequest& request,
		std::string_view token_handle,
		std::string_view auth_token,
		std::uint32_t* process_id,
		std::string* error_detail) override;

	[[nodiscard]] bool terminate_process(
		std::uint32_t process_id,
		std::string* error_detail) override;

	[[nodiscard]] bool is_process_running(std::uint32_t process_id) const override;

private:
	std::unordered_map<std::uint32_t, bool> running_;
	std::uint32_t next_process_id_ = 5000;
};

class HelperProcessLauncher {
public:
	explicit HelperProcessLauncher(std::shared_ptr<IHelperProcessLauncherAdapter> adapter = {});

	[[nodiscard]] HelperLaunchResult launch_helper(const HelperLaunchRequest& request);
	[[nodiscard]] bool stop_helper(std::string_view session_id, std::string* error_detail = nullptr);
	[[nodiscard]] bool is_helper_running(std::string_view session_id) const;
	[[nodiscard]] std::optional<std::uint32_t> helper_process_id(std::string_view session_id) const;

private:
	struct ActiveHelperEntry {
		std::uint32_t process_id = 0;
		std::string auth_token;
	};

	[[nodiscard]] static bool is_valid_request(const HelperLaunchRequest& request, std::string* error_detail);
	[[nodiscard]] std::string generate_auth_token(const std::string& session_id);

	std::shared_ptr<IHelperProcessLauncherAdapter> adapter_;
	std::unordered_map<std::string, ActiveHelperEntry> active_helpers_;
	std::uint64_t token_counter_ = 0;
};

}  // namespace redclaw::service

