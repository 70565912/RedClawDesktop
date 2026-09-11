#include "redclaw/service/capability_downgrade.h"
#include <cassert>
#include <iostream>

using namespace redclaw::service;

static void test_initial_state() {
  CapabilityDowngradeCoordinator coord;

  auto caps = coord.current_capabilities();
  assert(caps.session_mode == SessionMode::kPreLogin);
  assert(caps.secure_desktop_capture == true);

  std::cout << "✓ Initial state is PreLogin\n";
}

static void test_set_user_session_capabilities() {
  CapabilityDowngradeCoordinator coord;

  auto user_caps = build_user_session_capability_set();
  coord.set_current_capabilities(user_caps);

  auto caps = coord.current_capabilities();
  assert(caps.session_mode == SessionMode::kUserSession);
  assert(caps.user_desktop_input == true);

  std::cout << "✓ Can set UserSession capabilities\n";
}

static void test_downgrade_on_helper_lost() {
  CapabilityDowngradeCoordinator coord;
  coord.set_current_capabilities(build_user_session_capability_set());

  bool handler_called = false;
  DowngradeEvent captured_event;

  coord.set_downgrade_handler([&](const DowngradeEvent& event) {
    handler_called = true;
    captured_event = event;
  });

  bool result = coord.request_downgrade(DowngradeReason::HelperLost, "Helper crashed");
  assert(result);
  assert(handler_called);
  assert(captured_event.reason == DowngradeReason::HelperLost);
  assert(captured_event.from_capabilities.session_mode == SessionMode::kUserSession);
  assert(captured_event.to_capabilities.session_mode == SessionMode::kPreLogin);
  assert(captured_event.detail == "Helper crashed");

  auto caps = coord.current_capabilities();
  assert(caps.session_mode == SessionMode::kPreLogin);

  std::cout << "✓ Downgrade on helper lost\n";
}

static void test_downgrade_on_logout() {
  CapabilityDowngradeCoordinator coord;
  coord.set_current_capabilities(build_user_session_capability_set());

  bool handler_called = false;
  coord.set_downgrade_handler([&](const DowngradeEvent& event) {
    handler_called = true;
    assert(event.reason == DowngradeReason::LogoutDetected);
  });

  bool result = coord.request_downgrade(DowngradeReason::LogoutDetected);
  assert(result);
  assert(handler_called);

  auto caps = coord.current_capabilities();
  assert(caps.session_mode == SessionMode::kPreLogin);

  std::cout << "✓ Downgrade on logout detected\n";
}

static void test_downgrade_on_security_violation() {
  CapabilityDowngradeCoordinator coord;
  coord.set_current_capabilities(build_user_session_capability_set());

  bool handler_called = false;
  coord.set_downgrade_handler([&](const DowngradeEvent& event) {
    handler_called = true;
    assert(event.reason == DowngradeReason::SecurityBoundaryViolation);
  });

  bool result = coord.request_downgrade(DowngradeReason::SecurityBoundaryViolation,
                                        "UAC prompt detected");
  assert(result);
  assert(handler_called);

  auto caps = coord.current_capabilities();
  assert(caps.session_mode == SessionMode::kPreLogin);

  std::cout << "✓ Downgrade on security violation\n";
}

static void test_manual_downgrade() {
  CapabilityDowngradeCoordinator coord;
  coord.set_current_capabilities(build_user_session_capability_set());

  bool result = coord.request_downgrade(DowngradeReason::ManualRequest);
  assert(result);

  auto caps = coord.current_capabilities();
  assert(caps.session_mode == SessionMode::kPreLogin);

  std::cout << "✓ Manual downgrade request\n";
}

static void test_can_downgrade_validation() {
  CapabilityDowngradeCoordinator coord;
  coord.set_current_capabilities(build_user_session_capability_set());

  auto pre_login = build_pre_login_capability_set();
  assert(coord.can_downgrade_to(pre_login));

  auto secure_desktop = build_secure_desktop_capability_set();
  assert(coord.can_downgrade_to(secure_desktop));

  std::cout << "✓ Downgrade validation works\n";
}

static void test_no_handler_set() {
  CapabilityDowngradeCoordinator coord;
  coord.set_current_capabilities(build_user_session_capability_set());

  bool result = coord.request_downgrade(DowngradeReason::HelperLost);
  assert(result);

  auto caps = coord.current_capabilities();
  assert(caps.session_mode == SessionMode::kPreLogin);

  std::cout << "✓ Downgrade works without handler\n";
}

static void test_event_details() {
  CapabilityDowngradeCoordinator coord;
  coord.set_current_capabilities(build_user_session_capability_set());

  DowngradeEvent captured;
  coord.set_downgrade_handler([&](const DowngradeEvent& event) { captured = event; });

  coord.request_downgrade(DowngradeReason::SecurityBoundaryViolation, "IPC timeout after 30s");

  assert(captured.reason == DowngradeReason::SecurityBoundaryViolation);
  assert(captured.detail == "IPC timeout after 30s");
  assert(captured.from_capabilities.session_mode == SessionMode::kUserSession);
  assert(captured.to_capabilities.session_mode == SessionMode::kPreLogin);

  std::cout << "✓ Event details captured correctly\n";
}

static void test_multiple_downgrades() {
  CapabilityDowngradeCoordinator coord;

  coord.set_current_capabilities(build_user_session_capability_set());
  assert(coord.current_capabilities().session_mode == SessionMode::kUserSession);

  coord.request_downgrade(DowngradeReason::SecurityBoundaryViolation);
  assert(coord.current_capabilities().session_mode == SessionMode::kPreLogin);

  coord.request_downgrade(DowngradeReason::LogoutDetected);
  assert(coord.current_capabilities().session_mode == SessionMode::kPreLogin);

  std::cout << "✓ Multiple downgrades work\n";
}

static void test_handler_replacement() {
  CapabilityDowngradeCoordinator coord;
  coord.set_current_capabilities(build_user_session_capability_set());

  int handler1_calls = 0;
  coord.set_downgrade_handler([&](const DowngradeEvent&) { handler1_calls++; });

  coord.request_downgrade(DowngradeReason::HelperLost);
  assert(handler1_calls == 1);

  coord.set_current_capabilities(build_user_session_capability_set());

  int handler2_calls = 0;
  coord.set_downgrade_handler([&](const DowngradeEvent&) { handler2_calls++; });

  coord.request_downgrade(DowngradeReason::LogoutDetected);
  assert(handler1_calls == 1);
  assert(handler2_calls == 1);

  std::cout << "✓ Handler replacement works\n";
}

int main() {
  std::cout << "Running capability downgrade tests...\n";

  test_initial_state();
  test_set_user_session_capabilities();
  test_downgrade_on_helper_lost();
  test_downgrade_on_logout();
  test_downgrade_on_security_violation();
  test_manual_downgrade();
  test_can_downgrade_validation();
  test_no_handler_set();
  test_event_details();
  test_multiple_downgrades();
  test_handler_replacement();

  std::cout << "All capability downgrade tests passed!\n";
  return 0;
}
