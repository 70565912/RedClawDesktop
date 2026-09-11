#include "redclaw/helper/helper_capabilities.h"

#include <cassert>
#include <iostream>

using namespace redclaw::helper;

static void test_default_capability_set() {
  HelperCapabilityManager manager;
  const auto reg = manager.current_registration();

  assert((reg.advertised_flags & redclaw::service::HelperCapabilityFlag::kUserDesktopCapture) !=
         redclaw::service::HelperCapabilityFlag::kNone);
  assert(manager.is_enabled(HelperCapabilityDomain::kDesktopCapture));

  std::cout << "✓ Default capability set\n";
}

static void test_enable_disable_domains() {
  HelperCapabilityManager manager;
  manager.enable_domain(HelperCapabilityDomain::kClipboard);
  manager.enable_domain(HelperCapabilityDomain::kFileTransfer);

  assert(manager.is_enabled(HelperCapabilityDomain::kClipboard));
  assert(manager.is_enabled(HelperCapabilityDomain::kFileTransfer));

  manager.disable_domain(HelperCapabilityDomain::kClipboard);
  assert(!manager.is_enabled(HelperCapabilityDomain::kClipboard));
  assert(manager.is_enabled(HelperCapabilityDomain::kFileTransfer));

  std::cout << "✓ Enable/disable capability domains\n";
}

static void test_build_advertisement() {
  HelperCapabilityManager manager;
  manager.enable_domain(HelperCapabilityDomain::kDesktopInput);
  manager.enable_domain(HelperCapabilityDomain::kNotifications);
  manager.set_feature_flags({"feature.alpha", "feature.beta"});

  const auto ad = manager.build_advertisement("session-a", 42);
  assert(ad.session_id == "session-a");
  assert(ad.desktop_session_id == 42);
  assert(ad.feature_flags.size() == 2);
  assert((ad.available_capabilities & redclaw::service::HelperCapabilityFlag::kUserInputInjection) !=
         redclaw::service::HelperCapabilityFlag::kNone);

  std::cout << "✓ Build capability advertisement\n";
}

static void test_state_change_callback() {
  HelperCapabilityManager manager;
  int callback_count = 0;

  manager.set_capability_state_handler([&](const HelperCapabilityRegistration&) {
    callback_count++;
  });

  manager.enable_domain(HelperCapabilityDomain::kClipboard);
  manager.set_feature_flags({"f1"});
  manager.disable_domain(HelperCapabilityDomain::kClipboard);

  assert(callback_count >= 2);

  std::cout << "✓ Capability state callback\n";
}

static void test_idempotent_enable_disable() {
  HelperCapabilityManager manager;
  int callback_count = 0;
  manager.set_capability_state_handler([&](const HelperCapabilityRegistration&) {
    callback_count++;
  });

  const bool enabled_first = manager.enable_domain(HelperCapabilityDomain::kClipboard);
  const bool enabled_second = manager.enable_domain(HelperCapabilityDomain::kClipboard);
  assert(enabled_first);
  assert(enabled_second);
  assert(manager.is_enabled(HelperCapabilityDomain::kClipboard));

  // Second enable should be no-op (no extra callback).
  const int callbacks_after_enable = callback_count;
  manager.enable_domain(HelperCapabilityDomain::kClipboard);
  assert(callback_count == callbacks_after_enable);

  const bool disabled_first = manager.disable_domain(HelperCapabilityDomain::kClipboard);
  const bool disabled_second = manager.disable_domain(HelperCapabilityDomain::kClipboard);
  assert(disabled_first);
  assert(disabled_second);
  assert(!manager.is_enabled(HelperCapabilityDomain::kClipboard));

  std::cout << "✓ Idempotent enable/disable\n";
}

static void test_desktop_context_in_registration() {
  HelperCapabilityManager manager;
  manager.set_desktop_context("user-desktop");
  const auto reg = manager.current_registration();
  assert(reg.desktop_context == "user-desktop");

  const auto ad = manager.build_advertisement("session-b", 7);
  assert(ad.session_id == "session-b");
  assert(ad.desktop_session_id == 7);

  std::cout << "✓ Desktop context registration\n";
}

int main() {
  std::cout << "Running helper capability tests...\n";
  test_default_capability_set();
  test_enable_disable_domains();
  test_build_advertisement();
  test_state_change_callback();
  test_idempotent_enable_disable();
  test_desktop_context_in_registration();
  std::cout << "All helper capability tests passed!\n";
  return 0;
}
