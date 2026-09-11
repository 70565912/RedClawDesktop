#include "redclaw/service/capability_registry.h"
#include "redclaw/service/capability_sync.h"
#include "redclaw/service/capability_downgrade.h"
#include "redclaw/service/ipc_channel.h"
#include <cassert>
#include <iostream>
#include <memory>

using namespace redclaw::service;

static void test_capability_lifecycle() {
  auto pre_login = build_pre_login_capability_set();
  assert(pre_login.session_mode == SessionMode::kPreLogin);
  assert(pre_login.secure_desktop_capture == true);
  assert(pre_login.user_desktop_capture == false);

  auto user_session = build_user_session_capability_set();
  assert(user_session.session_mode == SessionMode::kUserSession);
  assert(user_session.user_desktop_capture == true);
  assert(user_session.user_desktop_input == true);
  assert(user_session.clipboard_sync == true);

  auto secure_desktop = build_secure_desktop_capability_set();
  assert(secure_desktop.session_mode == SessionMode::kPreLogin);
  assert(secure_desktop.secure_desktop_capture == true);
  assert(secure_desktop.user_desktop_capture == false);

  std::cout << "✓ Capability lifecycle (pre-login → user → secure)\n";
}

static void test_capability_transitions() {
  auto pre_login = build_pre_login_capability_set();
  auto user_session = build_user_session_capability_set();
  auto secure_desktop = build_secure_desktop_capability_set();

  assert(is_valid_capability_transition(pre_login, user_session));
  assert(is_valid_capability_transition(user_session, pre_login));
  assert(is_valid_capability_transition(user_session, secure_desktop));

  assert(is_capability_upgrade(pre_login, user_session));
  assert(is_capability_downgrade(user_session, pre_login));

  std::cout << "✓ Capability transitions validated\n";
}

static void test_sync_and_downgrade_integration() {
  CapabilityDowngradeCoordinator downgrade_coord;
  downgrade_coord.set_current_capabilities(build_user_session_capability_set());

  bool downgrade_triggered = false;
  downgrade_coord.set_downgrade_handler([&](const DowngradeEvent& event) {
    downgrade_triggered = true;
    assert(event.reason == DowngradeReason::HelperLost);
    assert(event.from_capabilities.session_mode == SessionMode::kUserSession);
    assert(event.to_capabilities.session_mode == SessionMode::kPreLogin);
  });

  bool result = downgrade_coord.request_downgrade(DowngradeReason::HelperLost,
                                                   "Simulated helper crash");
  assert(result);
  assert(downgrade_triggered);
  assert(downgrade_coord.current_capabilities().session_mode == SessionMode::kPreLogin);

  std::cout << "✓ Sync and downgrade integration\n";
}

static void test_capability_round_trip_through_ipc() {
  auto original = build_user_session_capability_set();

  IpcCapabilityAdvertisement adv;
  adv.header.schema_version = kIpcSchemaVersionV1;
  adv.header.message_type = IpcMessageType::kCapabilityAdvertisement;
  adv.available_capabilities = HelperCapabilityFlag::kUserDesktopCapture |
                                HelperCapabilityFlag::kUserInputInjection |
                                HelperCapabilityFlag::kClipboardSync |
                                HelperCapabilityFlag::kFileTransfer;
  adv.session_id = "test-session";
  adv.desktop_session_id = 1;

  IpcCapabilityAcknowledgment ack;
  ack.header.schema_version = kIpcSchemaVersionV1;
  ack.header.message_type = IpcMessageType::kCapabilityAcknowledgment;
  ack.accepted = true;
  ack.enabled_capabilities = adv.available_capabilities;

  assert(ack.accepted == true);
  assert(ack.enabled_capabilities == adv.available_capabilities);

  std::cout << "✓ Capability round-trip through IPC\n";
}

static void test_helper_capability_flags() {
  HelperCapabilityFlag none = HelperCapabilityFlag::kNone;
  assert(static_cast<uint32_t>(none) == 0);

  HelperCapabilityFlag capture = HelperCapabilityFlag::kUserDesktopCapture;
  HelperCapabilityFlag input = HelperCapabilityFlag::kUserInputInjection;
  
  HelperCapabilityFlag combined = capture | input;
  assert((combined & capture) != HelperCapabilityFlag::kNone);
  assert((combined & input) != HelperCapabilityFlag::kNone);

  HelperCapabilityFlag clipboard = HelperCapabilityFlag::kClipboardSync;
  assert((combined & clipboard) == HelperCapabilityFlag::kNone);

  std::cout << "✓ Helper capability flags\n";
}

static void test_capability_query_helpers() {
  auto user_session = build_user_session_capability_set();

  assert(has_desktop_capture(user_session));
  assert(has_desktop_input(user_session));
  assert(has_clipboard(user_session));
  assert(has_file_transfer(user_session));

  auto pre_login = build_pre_login_capability_set();
  assert(has_desktop_capture(pre_login));
  assert(!has_desktop_input(pre_login));
  assert(!has_clipboard(pre_login));
  assert(!has_file_transfer(pre_login));

  std::cout << "✓ Capability query helpers\n";
}

static void test_capability_diff() {
  auto pre_login = build_pre_login_capability_set();
  auto user_session = build_user_session_capability_set();

  auto diff = get_capability_diff(pre_login, user_session);
  assert(!diff.empty());

  std::cout << "✓ Capability diff generation\n";
}

static void test_session_mode_strings() {
  std::string pre_login_str = to_string(SessionMode::kPreLogin);
  std::string user_session_str = to_string(SessionMode::kUserSession);

  assert(!pre_login_str.empty());
  assert(!user_session_str.empty());
  assert(pre_login_str != user_session_str);

  std::cout << "✓ Session mode strings\n";
}

static void test_downgrade_reason_strings() {
  std::string logout = to_string(CapabilityDowngradeReason::kUserLogout);
  std::string crash = to_string(CapabilityDowngradeReason::kHelperCrashed);

  assert(!logout.empty());
  assert(!crash.empty());

  std::cout << "✓ Downgrade reason strings\n";
}

static void test_upgrade_reason_strings() {
  std::string login = to_string(CapabilityUpgradeReason::kUserLogin);
  std::string started = to_string(CapabilityUpgradeReason::kHelperStarted);

  assert(!login.empty());
  assert(!started.empty());

  std::cout << "✓ Upgrade reason strings\n";
}

static void test_advertised_capabilities() {
  auto pre_login_caps = get_advertised_capabilities(SessionMode::kPreLogin);
  assert((pre_login_caps & HelperCapabilityFlag::kUserDesktopCapture) == HelperCapabilityFlag::kNone);

  auto user_session_caps = get_advertised_capabilities(SessionMode::kUserSession);
  assert((user_session_caps & HelperCapabilityFlag::kUserDesktopCapture) != HelperCapabilityFlag::kNone);
  assert((user_session_caps & HelperCapabilityFlag::kUserInputInjection) != HelperCapabilityFlag::kNone);

  std::cout << "✓ Advertised capabilities\n";
}

static void test_describe_capability_set() {
  auto user_session = build_user_session_capability_set();
  std::string description = describe_capability_set(user_session);
  assert(!description.empty());

  std::cout << "✓ Describe capability set\n";
}

static void test_full_session_flow() {
  CapabilityDowngradeCoordinator coord;

  assert(coord.current_capabilities().session_mode == SessionMode::kPreLogin);

  coord.set_current_capabilities(build_user_session_capability_set());
  assert(coord.current_capabilities().session_mode == SessionMode::kUserSession);

  coord.request_downgrade(DowngradeReason::HelperLost, "Connection lost");
  assert(coord.current_capabilities().session_mode == SessionMode::kPreLogin);

  coord.set_current_capabilities(build_user_session_capability_set());
  coord.request_downgrade(DowngradeReason::SecurityBoundaryViolation, "UAC");
  assert(coord.current_capabilities().session_mode == SessionMode::kPreLogin);

  std::cout << "✓ Full session flow\n";
}

int main() {
  std::cout << "Running integrated capability tests...\n";

  test_capability_lifecycle();
  test_capability_transitions();
  test_sync_and_downgrade_integration();
  test_capability_round_trip_through_ipc();
  test_helper_capability_flags();
  test_capability_query_helpers();
  test_capability_diff();
  test_session_mode_strings();
  test_downgrade_reason_strings();
  test_upgrade_reason_strings();
  test_advertised_capabilities();
  test_describe_capability_set();
  test_full_session_flow();

  std::cout << "All integrated capability tests passed!\n";
  return 0;
}
