#include "redclaw/helper/helper_bootstrap.h"
#include <Windows.h>
#include <iostream>
#include <string>

using namespace redclaw::helper;

namespace {

bool g_running = true;

BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
  if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT || 
      ctrl_type == CTRL_CLOSE_EVENT) {
    std::cout << "Shutdown signal received\n";
    g_running = false;
    return TRUE;
  }
  return FALSE;
}

std::string get_env_var(const char* name, const std::string& default_value = "") {
  char buffer[512];
  DWORD result = ::GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  if (result == 0 || result >= sizeof(buffer)) {
    return default_value;
  }
  return std::string(buffer, result);
}

}  // namespace

int main(int argc, char* argv[]) {
  std::cout << "RedClaw User-Session Helper starting...\n";

  ::SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

  HelperBootstrapConfig config;
  config.auth_token = get_env_var("REDCLAW_AUTH_TOKEN", "default-auth-token");
  config.session_id = get_env_var("REDCLAW_SESSION_ID", "session-0");
  
  if (argc > 1) {
    config.service_pipe_name = argv[1];
  }

  std::cout << "  Session ID: " << config.session_id << "\n";
  std::cout << "  Service Pipe: " << config.service_pipe_name << "\n";

  HelperBootstrap bootstrap(config);

  bootstrap.set_state_change_handler([](HelperBootstrapState state, const std::string& detail) {
    std::cout << "State: ";
    switch (state) {
      case HelperBootstrapState::Uninitialized:
        std::cout << "Uninitialized";
        break;
      case HelperBootstrapState::Connecting:
        std::cout << "Connecting";
        break;
      case HelperBootstrapState::Authenticating:
        std::cout << "Authenticating";
        break;
      case HelperBootstrapState::CapabilityRegistration:
        std::cout << "CapabilityRegistration";
        break;
      case HelperBootstrapState::Ready:
        std::cout << "Ready";
        break;
      case HelperBootstrapState::Disconnected:
        std::cout << "Disconnected";
        break;
      case HelperBootstrapState::Failed:
        std::cout << "Failed";
        break;
    }
    if (!detail.empty()) {
      std::cout << " - " << detail;
    }
    std::cout << "\n";
  });

  bootstrap.set_error_handler([](const std::string& error) {
    std::cerr << "ERROR: " << error << "\n";
  });

  if (!bootstrap.initialize()) {
    std::cerr << "Failed to initialize helper\n";
    return 1;
  }

  std::cout << "Helper ready. Press Ctrl+C to exit.\n";
  std::cout << "Helper Process ID: " << bootstrap.helper_process_id() << "\n";

  while (g_running && bootstrap.state() == HelperBootstrapState::Ready) {
    ::Sleep(100);
  }

  std::cout << "Shutting down...\n";
  bootstrap.shutdown();
  std::cout << "Helper terminated.\n";

  return 0;
}
