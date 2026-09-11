#include "redclaw/helper/helper_capabilities.h"

#include <algorithm>

namespace redclaw::helper {

HelperCapabilityManager::HelperCapabilityManager() {
  enable_domain(HelperCapabilityDomain::kDesktopCapture);
}

void HelperCapabilityManager::set_capability_state_handler(CapabilityStateHandler handler) {
  capability_state_handler_ = std::move(handler);
}

void HelperCapabilityManager::set_feature_flags(std::vector<std::string> feature_flags) {
  registration_.feature_flags = std::move(feature_flags);
  notify_state_changed();
}

void HelperCapabilityManager::set_desktop_context(std::string desktop_context) {
  registration_.desktop_context = std::move(desktop_context);
  notify_state_changed();
}

bool HelperCapabilityManager::enable_domain(HelperCapabilityDomain domain) {
  if (contains_domain(registration_.active_domains, domain)) {
    return true;
  }

  registration_.active_domains.push_back(domain);
  registration_.advertised_flags = registration_.advertised_flags | to_flag(domain);
  notify_state_changed();
  return true;
}

bool HelperCapabilityManager::disable_domain(HelperCapabilityDomain domain) {
  if (!contains_domain(registration_.active_domains, domain)) {
    return true;
  }

  remove_domain(registration_.active_domains, domain);
  registration_.advertised_flags = service::HelperCapabilityFlag::kNone;
  for (const auto active_domain : registration_.active_domains) {
    registration_.advertised_flags = registration_.advertised_flags | to_flag(active_domain);
  }

  notify_state_changed();
  return true;
}

bool HelperCapabilityManager::is_enabled(HelperCapabilityDomain domain) const {
  return contains_domain(registration_.active_domains, domain);
}

HelperCapabilityRegistration HelperCapabilityManager::current_registration() const {
  return registration_;
}

service::IpcCapabilityAdvertisement HelperCapabilityManager::build_advertisement(
  const std::string& session_id,
  std::uint32_t desktop_session_id) const {
  service::IpcCapabilityAdvertisement advertisement;
  advertisement.header.schema_version = service::kIpcSchemaVersionV1;
  advertisement.header.message_type = service::IpcMessageType::kCapabilityAdvertisement;
  advertisement.session_id = session_id;
  advertisement.available_capabilities = registration_.advertised_flags;
  advertisement.feature_flags = registration_.feature_flags;
  advertisement.desktop_session_id = desktop_session_id;
  return advertisement;
}

void HelperCapabilityManager::notify_state_changed() const {
  if (capability_state_handler_) {
    capability_state_handler_(registration_);
  }
}

service::HelperCapabilityFlag HelperCapabilityManager::to_flag(HelperCapabilityDomain domain) {
  switch (domain) {
    case HelperCapabilityDomain::kDesktopCapture:
      return service::HelperCapabilityFlag::kUserDesktopCapture;
    case HelperCapabilityDomain::kDesktopInput:
      return service::HelperCapabilityFlag::kUserInputInjection;
    case HelperCapabilityDomain::kClipboard:
      return service::HelperCapabilityFlag::kClipboardSync;
    case HelperCapabilityDomain::kFileTransfer:
      return service::HelperCapabilityFlag::kFileTransfer;
    case HelperCapabilityDomain::kNotifications:
      return service::HelperCapabilityFlag::kNotifications;
  }

  return service::HelperCapabilityFlag::kNone;
}

bool HelperCapabilityManager::contains_domain(
  const std::vector<HelperCapabilityDomain>& domains,
  HelperCapabilityDomain domain) {
  return std::find(domains.begin(), domains.end(), domain) != domains.end();
}

void HelperCapabilityManager::remove_domain(
  std::vector<HelperCapabilityDomain>& domains,
  HelperCapabilityDomain domain) {
  domains.erase(std::remove(domains.begin(), domains.end(), domain), domains.end());
}

}  // namespace redclaw::helper
