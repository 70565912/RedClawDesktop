#include "redclaw/helper/helper_bootstrap.h"
#include "redclaw/service/ipc_channel.h"
#include <cassert>
#include <iostream>
#include <thread>

using namespace redclaw::helper;
using namespace redclaw::service;

static void test_helper_bootstrap_initialization() {
  HelperBootstrapConfig config;
  config.auth_token = "test-token";
  config.session_id = "test-session";

  HelperBootstrap bootstrap(config);
  assert(bootstrap.state() == HelperBootstrapState::Uninitialized);
  assert(bootstrap.session_id() == "test-session");
  assert(!bootstrap.helper_process_id().empty());

  std::cout << "✓ Helper bootstrap initialization\n";
}

static void test_helper_process_id_generation() {
  HelperBootstrapConfig config1;
  config1.session_id = "session-1";
  
  HelperBootstrapConfig config2;
  config2.session_id = "session-2";

  HelperBootstrap bootstrap1(config1);
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  HelperBootstrap bootstrap2(config2);

  std::string id1 = bootstrap1.helper_process_id();
  std::string id2 = bootstrap2.helper_process_id();

  assert(!id1.empty());
  assert(!id2.empty());
  assert(id1 != id2);

  std::cout << "✓ Helper process ID generation\n";
}

static void test_configuration() {
  HelperBootstrapConfig config;
  config.service_pipe_name = "\\\\.\\pipe\\custom_pipe";
  config.auth_token = "custom-token";
  config.session_id = "custom-session";
  config.connect_timeout_ms = 10000;
  config.handshake_timeout_ms = 5000;
  config.keepalive_interval_ms = 250;

  HelperBootstrap bootstrap(config);
  assert(bootstrap.session_id() == "custom-session");
  assert(bootstrap.state() == HelperBootstrapState::Uninitialized);
  assert(!bootstrap.is_keepalive_running());

  std::cout << "✓ Configuration\n";
}

static void test_multiple_instances() {
  HelperBootstrapConfig config1;
  config1.session_id = "session-1";
  
  HelperBootstrapConfig config2;
  config2.session_id = "session-2";

  HelperBootstrap bootstrap1(config1);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  HelperBootstrap bootstrap2(config2);

  assert(bootstrap1.session_id() == "session-1");
  assert(bootstrap2.session_id() == "session-2");
  assert(!bootstrap1.helper_process_id().empty());
  assert(!bootstrap2.helper_process_id().empty());

  std::cout << "✓ Multiple instances\n";
}

static void test_keepalive_initial_state() {
  HelperBootstrapConfig config;
  config.session_id = "session-ka";
  config.keepalive_interval_ms = 100;

  HelperBootstrap bootstrap(config);
  assert(!bootstrap.is_keepalive_running());

  std::cout << "✓ Keepalive initial state\n";
}

static void test_state_transitions() {
  HelperBootstrapConfig config;
  config.service_pipe_name = "\\\\.\\pipe\\nonexistent_test";
  config.connect_timeout_ms = 100;

  HelperBootstrap bootstrap(config);

  std::vector<HelperBootstrapState> state_history;
  bootstrap.set_state_change_handler(
      [&](HelperBootstrapState state, const std::string&) {
        state_history.push_back(state);
      });

  bool result = bootstrap.initialize();
  assert(!result);
  assert(!state_history.empty());
  assert(state_history.front() == HelperBootstrapState::Connecting);
  assert(state_history.back() == HelperBootstrapState::Failed);

  std::cout << "✓ State transitions on connection failure\n";
}

static void test_error_handler() {
  HelperBootstrapConfig config;
  config.service_pipe_name = "\\\\.\\pipe\\nonexistent_pipe_xyz";
  config.connect_timeout_ms = 100;

  HelperBootstrap bootstrap(config);

  bool error_reported = false;
  std::string error_message;

  bootstrap.set_error_handler([&](const std::string& error) {
    error_reported = true;
    error_message = error;
  });

  bool result = bootstrap.initialize();
  assert(!result);
  assert(error_reported);
  assert(!error_message.empty());

  std::cout << "✓ Error handler\n";
}

static void test_shutdown_after_failed_initialize() {
  HelperBootstrapConfig config;
  config.service_pipe_name = "\\\\.\\pipe\\nonexistent_pipe_shutdown";
  config.connect_timeout_ms = 100;

  HelperBootstrap bootstrap(config);
  assert(!bootstrap.initialize());
  assert(bootstrap.state() == HelperBootstrapState::Failed);

  bootstrap.shutdown();
  assert(bootstrap.state() == HelperBootstrapState::Disconnected);
  assert(!bootstrap.is_keepalive_running());

  std::cout << "✓ Shutdown after failed initialize\n";
}

static void test_shutdown_is_idempotent() {
  HelperBootstrapConfig config;
  config.service_pipe_name = "\\\\.\\pipe\\nonexistent_pipe_shutdown_twice";
  config.connect_timeout_ms = 100;

  HelperBootstrap bootstrap(config);
  (void)bootstrap.initialize();  // Expected to fail in in-memory test setup

  bootstrap.shutdown();
  const auto state_after_first = bootstrap.state();
  bootstrap.shutdown();

  assert(state_after_first == HelperBootstrapState::Disconnected);
  assert(bootstrap.state() == HelperBootstrapState::Disconnected);
  assert(!bootstrap.is_keepalive_running());

  std::cout << "✓ Shutdown idempotency\n";
}

int main() {
  std::cout << "Running helper bootstrap tests...\n";

  test_helper_bootstrap_initialization();
  test_helper_process_id_generation();
  test_configuration();
  test_multiple_instances();
  test_keepalive_initial_state();
  test_state_transitions();
  test_error_handler();
  test_shutdown_after_failed_initialize();
  test_shutdown_is_idempotent();

  std::cout << "All helper bootstrap tests passed!\n";
  return 0;
}
