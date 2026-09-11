#ifndef REDCLAW_HELPER_RUNTIME_PROFILE_H
#define REDCLAW_HELPER_RUNTIME_PROFILE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace redclaw::helper {

struct RuntimeProfileData {
  std::optional<std::string> role;
  std::optional<std::string> target_host;
  std::optional<std::uint16_t> target_port;
  std::optional<std::string> rendezvous_url;
  std::optional<std::string> session_code;
  std::optional<std::uint32_t> run_seconds;
  std::optional<std::uint32_t> signal_timeout_seconds;
  std::optional<std::string> signal_dir;
  std::optional<std::string> signal_transport;
  std::optional<std::string> signal_passphrase;
  std::vector<std::string> dht_bootstrap_nodes;
  std::optional<std::string> network_bind_address;
  std::optional<std::uint16_t> ice_udp_port;
  std::optional<std::uint16_t> dht_listen_port;
  std::optional<std::uint32_t> dht_poll_interval_ms;
  std::optional<std::uint32_t> dht_publish_retry_ms;
  std::optional<bool> enable_port_mapping;
  std::optional<bool> enable_ipv6_candidates;
  std::optional<bool> allow_remote_input;
  std::optional<bool> allow_remote_agent;
  std::optional<std::string> agent_project_manifest;
  std::optional<std::string> helper_role;
  std::vector<std::string> ice_servers;
  std::optional<bool> enable_ice_tcp;
  std::optional<bool> stream_smoke;
  std::optional<bool> stream_require_capture;
  std::optional<std::uint32_t> stream_preview_width;
  std::optional<std::uint32_t> stream_video_max_width;
  std::optional<bool> cli;
};

bool load_runtime_profile_file(
  const std::string& config_path,
  RuntimeProfileData* profile,
  std::string* error_detail = nullptr);

}  // namespace redclaw::helper

#endif  // REDCLAW_HELPER_RUNTIME_PROFILE_H
