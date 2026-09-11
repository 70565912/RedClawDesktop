#ifndef REDCLAW_HELPER_HELPER_BOOTSTRAP_H
#define REDCLAW_HELPER_HELPER_BOOTSTRAP_H

#include "redclaw/service/ipc_channel.h"
#include "redclaw/helper/helper_capabilities.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace redclaw {
namespace helper {

enum class HelperBootstrapState {
  Uninitialized,
  Connecting,
  Authenticating,
  CapabilityRegistration,
  Ready,
  Disconnected,
  Failed
};

struct HelperBootstrapConfig {
  std::string service_pipe_name = "\\\\.\\pipe\\redclaw_service_ipc";
  std::string auth_token;
  std::string session_id;
  uint32_t connect_timeout_ms = 5000;
  uint32_t handshake_timeout_ms = 3000;
  uint32_t keepalive_interval_ms = 1000;
};

using BootstrapStateChangeHandler = std::function<void(HelperBootstrapState, const std::string&)>;
using BootstrapErrorHandler = std::function<void(const std::string&)>;

class HelperBootstrap {
 public:
  explicit HelperBootstrap(const HelperBootstrapConfig& config);
  ~HelperBootstrap();

  HelperBootstrap(const HelperBootstrap&) = delete;
  HelperBootstrap& operator=(const HelperBootstrap&) = delete;

  bool initialize();
  
  void shutdown();

  HelperBootstrapState state() const;
  bool is_keepalive_running() const;

  void set_state_change_handler(BootstrapStateChangeHandler handler);
  void set_error_handler(BootstrapErrorHandler handler);

  std::string session_id() const;
  std::string helper_process_id() const;
  const HelperCapabilityManager& capability_manager() const;

 private:
  void transition_to(HelperBootstrapState new_state, const std::string& detail = "");
  void report_error(const std::string& error);

  bool connect_to_service();
  bool perform_handshake();
  bool register_capabilities();
  bool start_keepalive();
  void stop_keepalive();
  void run_keepalive_loop();

  HelperBootstrapConfig config_;
  HelperBootstrapState state_;
  std::unique_ptr<service::WindowsNamedPipeIpcChannel> ipc_channel_;
  HelperCapabilityManager capability_manager_;
  BootstrapStateChangeHandler state_change_handler_;
  BootstrapErrorHandler error_handler_;
  std::string helper_process_id_;
  std::atomic<bool> keepalive_running_ {false};
  std::atomic<bool> shutdown_requested_ {false};
  std::thread keepalive_thread_;
};

}  // namespace helper
}  // namespace redclaw

#endif  // REDCLAW_HELPER_HELPER_BOOTSTRAP_H
