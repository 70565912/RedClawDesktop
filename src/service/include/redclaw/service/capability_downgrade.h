#ifndef REDCLAW_SERVICE_CAPABILITY_DOWNGRADE_H
#define REDCLAW_SERVICE_CAPABILITY_DOWNGRADE_H

#include "capability_registry.h"
#include <functional>
#include <string>

namespace redclaw {
namespace service {

enum class DowngradeReason {
  HelperLost,
  LogoutDetected,
  SecurityBoundaryViolation,
  ManualRequest
};

struct DowngradeEvent {
  DowngradeReason reason;
  CapabilitySet from_capabilities;
  CapabilitySet to_capabilities;
  std::string detail;
};

using DowngradeHandler = std::function<void(const DowngradeEvent&)>;

class CapabilityDowngradeCoordinator {
 public:
  CapabilityDowngradeCoordinator();

  void set_current_capabilities(const CapabilitySet& caps);
  CapabilitySet current_capabilities() const;

  void set_downgrade_handler(DowngradeHandler handler);

  bool request_downgrade(DowngradeReason reason, const std::string& detail = "");

  bool can_downgrade_to(const CapabilitySet& target) const;

 private:
  void execute_downgrade(const CapabilitySet& target, DowngradeReason reason,
                         const std::string& detail);

  CapabilitySet current_caps_;
  DowngradeHandler downgrade_handler_;
};

}  // namespace service
}  // namespace redclaw

#endif  // REDCLAW_SERVICE_CAPABILITY_DOWNGRADE_H
