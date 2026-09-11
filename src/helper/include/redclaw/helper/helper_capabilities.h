#ifndef REDCLAW_HELPER_HELPER_CAPABILITIES_H
#define REDCLAW_HELPER_HELPER_CAPABILITIES_H

#include "redclaw/service/ipc_channel_contracts.h"
#include <functional>
#include <string>
#include <vector>

namespace redclaw::helper {

enum class HelperCapabilityDomain {
  kDesktopCapture,
  kDesktopInput,
  kClipboard,
  kFileTransfer,
  kNotifications,
};

struct HelperCapabilityRegistration {
  service::HelperCapabilityFlag advertised_flags = service::HelperCapabilityFlag::kNone;
  std::vector<HelperCapabilityDomain> active_domains;
  std::vector<std::string> feature_flags;
  std::string desktop_context;
};

using CapabilityStateHandler = std::function<void(const HelperCapabilityRegistration&)>;

class HelperCapabilityManager {
public:
  HelperCapabilityManager();

  void set_capability_state_handler(CapabilityStateHandler handler);
  void set_feature_flags(std::vector<std::string> feature_flags);
  void set_desktop_context(std::string desktop_context);

  bool enable_domain(HelperCapabilityDomain domain);
  bool disable_domain(HelperCapabilityDomain domain);
  bool is_enabled(HelperCapabilityDomain domain) const;

  [[nodiscard]] HelperCapabilityRegistration current_registration() const;
  [[nodiscard]] service::IpcCapabilityAdvertisement build_advertisement(
    const std::string& session_id,
    std::uint32_t desktop_session_id) const;

private:
  void notify_state_changed() const;
  static service::HelperCapabilityFlag to_flag(HelperCapabilityDomain domain);
  static bool contains_domain(const std::vector<HelperCapabilityDomain>& domains, HelperCapabilityDomain domain);
  static void remove_domain(std::vector<HelperCapabilityDomain>& domains, HelperCapabilityDomain domain);

  HelperCapabilityRegistration registration_;
  CapabilityStateHandler capability_state_handler_;
};

}  // namespace redclaw::helper

#endif  // REDCLAW_HELPER_HELPER_CAPABILITIES_H
