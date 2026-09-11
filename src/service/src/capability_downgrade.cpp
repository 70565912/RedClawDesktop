#include "redclaw/service/capability_downgrade.h"

namespace redclaw {
namespace service {

CapabilityDowngradeCoordinator::CapabilityDowngradeCoordinator()
    : current_caps_(build_pre_login_capability_set()), downgrade_handler_(nullptr) {}

void CapabilityDowngradeCoordinator::set_current_capabilities(const CapabilitySet& caps) {
  current_caps_ = caps;
}

CapabilitySet CapabilityDowngradeCoordinator::current_capabilities() const {
  return current_caps_;
}

void CapabilityDowngradeCoordinator::set_downgrade_handler(DowngradeHandler handler) {
  downgrade_handler_ = std::move(handler);
}

bool CapabilityDowngradeCoordinator::request_downgrade(DowngradeReason reason,
                                                        const std::string& detail) {
  CapabilitySet target;

  switch (reason) {
    case DowngradeReason::HelperLost:
    case DowngradeReason::LogoutDetected:
      target = build_pre_login_capability_set();
      break;

    case DowngradeReason::SecurityBoundaryViolation:
      target = build_secure_desktop_capability_set();
      break;

    case DowngradeReason::ManualRequest:
      target = build_pre_login_capability_set();
      break;

    default:
      return false;
  }

  if (!can_downgrade_to(target)) {
    return false;
  }

  execute_downgrade(target, reason, detail);
  return true;
}

bool CapabilityDowngradeCoordinator::can_downgrade_to(const CapabilitySet& target) const {
  return is_valid_capability_transition(current_caps_, target);
}

void CapabilityDowngradeCoordinator::execute_downgrade(const CapabilitySet& target,
                                                        DowngradeReason reason,
                                                        const std::string& detail) {
  DowngradeEvent event;
  event.reason = reason;
  event.from_capabilities = current_caps_;
  event.to_capabilities = target;
  event.detail = detail;

  current_caps_ = target;

  if (downgrade_handler_) {
    downgrade_handler_(event);
  }
}

}  // namespace service
}  // namespace redclaw
