#include "redclaw/helper/helper_bootstrap.h"
#include "redclaw/service/ipc_channel.h"
#include "redclaw/service/ipc_channel_contracts.h"
#include <Windows.h>
#include <Wtsapi32.h>
#include <chrono>
#include <sstream>
#include <thread>

namespace redclaw {
namespace helper {

namespace {
std::string generate_helper_process_id() {
  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  std::ostringstream oss;
  oss << "helper-" << ::GetCurrentProcessId() << "-" << ms;
  return oss.str();
}
}  // namespace

HelperBootstrap::HelperBootstrap(const HelperBootstrapConfig& config)
    : config_(config),
      state_(HelperBootstrapState::Uninitialized),
      state_change_handler_(nullptr),
      error_handler_(nullptr),
      helper_process_id_(generate_helper_process_id()) {}

HelperBootstrap::~HelperBootstrap() {
  shutdown();
}

bool HelperBootstrap::initialize() {
  if (state_ != HelperBootstrapState::Uninitialized) {
    report_error("Already initialized");
    return false;
  }

  if (!connect_to_service()) {
    transition_to(HelperBootstrapState::Failed, "Connection failed");
    return false;
  }

  if (!perform_handshake()) {
    transition_to(HelperBootstrapState::Failed, "Handshake failed");
    return false;
  }

  if (!register_capabilities()) {
    transition_to(HelperBootstrapState::Failed, "Capability registration failed");
    return false;
  }

  if (!start_keepalive()) {
    transition_to(HelperBootstrapState::Failed, "Keepalive startup failed");
    return false;
  }

  transition_to(HelperBootstrapState::Ready, "Bootstrap complete");
  return true;
}

void HelperBootstrap::shutdown() {
  if (state_ == HelperBootstrapState::Uninitialized || 
      state_ == HelperBootstrapState::Disconnected) {
    return;
  }

  shutdown_requested_.store(true);
  stop_keepalive();

  if (ipc_channel_) {
    service::IpcShutdown shutdown_msg;
    shutdown_msg.header.message_type = service::IpcMessageType::kShutdown;
    shutdown_msg.header.schema_version = service::kIpcSchemaVersionV1;
    shutdown_msg.session_id = config_.session_id;
    shutdown_msg.reason_code = "helper-shutdown";
    shutdown_msg.graceful = true;
    
    std::string serialized = service::serialize_ipc_shutdown(shutdown_msg);
    (void)ipc_channel_->send_message(serialized);
    
    (void)ipc_channel_->disconnect();
    ipc_channel_.reset();
  }

  transition_to(HelperBootstrapState::Disconnected, "Shutdown complete");
}

HelperBootstrapState HelperBootstrap::state() const {
  return state_;
}

bool HelperBootstrap::is_keepalive_running() const {
  return keepalive_running_.load();
}

void HelperBootstrap::set_state_change_handler(BootstrapStateChangeHandler handler) {
  state_change_handler_ = std::move(handler);
}

void HelperBootstrap::set_error_handler(BootstrapErrorHandler handler) {
  error_handler_ = std::move(handler);
}

std::string HelperBootstrap::session_id() const {
  return config_.session_id;
}

std::string HelperBootstrap::helper_process_id() const {
  return helper_process_id_;
}

const HelperCapabilityManager& HelperBootstrap::capability_manager() const {
  return capability_manager_;
}

void HelperBootstrap::transition_to(HelperBootstrapState new_state, const std::string& detail) {
  state_ = new_state;
  if (state_change_handler_) {
    state_change_handler_(new_state, detail);
  }
}

void HelperBootstrap::report_error(const std::string& error) {
  if (error_handler_) {
    error_handler_(error);
  }
}

bool HelperBootstrap::connect_to_service() {
  transition_to(HelperBootstrapState::Connecting, "Connecting to service");

  service::IpcChannelConfig ipc_config;
  ipc_config.pipe_name = config_.service_pipe_name;
  ipc_config.timeout_ms = config_.connect_timeout_ms;

  ipc_channel_ = std::make_unique<service::WindowsNamedPipeIpcChannel>(ipc_config);

  if (!ipc_channel_->connect_to_server()) {
    report_error("Failed to connect to service pipe");
    return false;
  }

  auto start = std::chrono::steady_clock::now();
  while (ipc_channel_->state() != service::IpcChannelState::kConnected) {
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed > std::chrono::milliseconds(config_.connect_timeout_ms)) {
      report_error("Connection timeout");
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return true;
}

bool HelperBootstrap::perform_handshake() {
  transition_to(HelperBootstrapState::Authenticating, "Performing handshake");

  service::IpcHandshakeRequest handshake;
  handshake.header.message_type = service::IpcMessageType::kHandshakeRequest;
  handshake.header.schema_version = service::kIpcSchemaVersionV1;
  handshake.service_auth_token = config_.auth_token;
  handshake.session_id = config_.session_id;
  handshake.helper_process_id = helper_process_id_;
  
  auto now = std::chrono::system_clock::now();
  handshake.helper_start_time_ms = 
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

  std::string serialized = service::serialize_ipc_handshake_request(handshake);
  if (!ipc_channel_->send_message(serialized)) {
    report_error("Failed to send handshake request");
    return false;
  }

  auto start = std::chrono::steady_clock::now();
  while (true) {
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed > std::chrono::milliseconds(config_.handshake_timeout_ms)) {
      report_error("Handshake timeout");
      return false;
    }

    std::string response;
    if (ipc_channel_->receive_message(response)) {
      auto parse_result = service::parse_ipc_handshake_response(response);
      if (parse_result.ok) {
        if (parse_result.message.authenticated) {
          return true;
        } else {
          report_error("Authentication failed");
          return false;
        }
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool HelperBootstrap::register_capabilities() {
  transition_to(HelperBootstrapState::CapabilityRegistration, "Registering capabilities");

  capability_manager_.enable_domain(HelperCapabilityDomain::kDesktopInput);
  capability_manager_.enable_domain(HelperCapabilityDomain::kClipboard);
  capability_manager_.enable_domain(HelperCapabilityDomain::kFileTransfer);
  capability_manager_.enable_domain(HelperCapabilityDomain::kNotifications);
  capability_manager_.set_feature_flags({"helper.cap.capture", "helper.cap.input", "helper.cap.clipboard"});
  capability_manager_.set_desktop_context("user-desktop");

  const auto adv = capability_manager_.build_advertisement(
    config_.session_id,
    ::WTSGetActiveConsoleSessionId());

  const std::string serialized = service::serialize_ipc_capability_advertisement(adv);
  if (!ipc_channel_->send_message(serialized)) {
    report_error("Failed to send capability advertisement");
    return false;
  }

  auto start = std::chrono::steady_clock::now();
  while (true) {
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed > std::chrono::milliseconds(config_.handshake_timeout_ms)) {
      report_error("Capability registration timeout");
      return false;
    }

    std::string response;
    if (ipc_channel_->receive_message(response)) {
      auto parse_result = service::parse_ipc_capability_acknowledgment(response);
      if (parse_result.ok) {
        if (parse_result.message.accepted) {
          return true;
        } else {
          report_error("Capability registration rejected");
          return false;
        }
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool HelperBootstrap::start_keepalive() {
  if (keepalive_running_.load()) {
    return true;
  }

  if (!ipc_channel_ || !ipc_channel_->is_connected()) {
    report_error("Cannot start keepalive without active IPC connection");
    return false;
  }

  shutdown_requested_.store(false);
  keepalive_running_.store(true);
  keepalive_thread_ = std::thread(&HelperBootstrap::run_keepalive_loop, this);
  return true;
}

void HelperBootstrap::stop_keepalive() {
  keepalive_running_.store(false);
  if (keepalive_thread_.joinable()) {
    keepalive_thread_.join();
  }
}

void HelperBootstrap::run_keepalive_loop() {
  while (keepalive_running_.load() && !shutdown_requested_.load()) {
    if (!ipc_channel_ || !ipc_channel_->is_connected()) {
      report_error("Keepalive stopped: IPC disconnected");
      keepalive_running_.store(false);
      break;
    }

    service::IpcKeepalive keepalive;
    keepalive.header.message_type = service::IpcMessageType::kKeepalive;
    keepalive.header.schema_version = service::kIpcSchemaVersionV1;
    keepalive.session_id = config_.session_id;

    const std::string payload = service::serialize_ipc_keepalive(keepalive);
    if (!ipc_channel_->send_message(payload)) {
      report_error("Failed to send keepalive");
      keepalive_running_.store(false);
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(config_.keepalive_interval_ms));
  }
}

}  // namespace helper
}  // namespace redclaw
