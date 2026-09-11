#include "redclaw/helper/runtime_profile.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace redclaw::helper {

namespace {

std::string trim_copy(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }

  std::size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }

  return std::string(value.substr(begin, end - begin));
}

std::string to_lower_ascii(std::string_view value) {
  std::string lower;
  lower.reserve(value.size());
  for (const char ch : value) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return lower;
}

bool parse_uint16(std::string_view value, std::uint16_t* out) {
  if (value.empty()) {
    return false;
  }

  std::uint32_t parsed = 0;
  for (const char ch : value) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    parsed = (parsed * 10U) + static_cast<std::uint32_t>(ch - '0');
    if (parsed > 65535U) {
      return false;
    }
  }

  *out = static_cast<std::uint16_t>(parsed);
  return true;
}

bool parse_uint32(std::string_view value, std::uint32_t* out) {
  if (value.empty()) {
    return false;
  }

  std::uint64_t parsed = 0;
  for (const char ch : value) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    parsed = (parsed * 10ULL) + static_cast<std::uint64_t>(ch - '0');
    if (parsed > static_cast<std::uint64_t>(UINT32_MAX)) {
      return false;
    }
  }

  *out = static_cast<std::uint32_t>(parsed);
  return true;
}

bool parse_bool_token(std::string_view value, bool* out) {
  const std::string lower = to_lower_ascii(value);
  if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
    *out = true;
    return true;
  }
  if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
    *out = false;
    return true;
  }
  return false;
}

}  // namespace

bool load_runtime_profile_file(
  const std::string& config_path,
  RuntimeProfileData* profile,
  std::string* error_detail) {
  if (profile == nullptr) {
    if (error_detail != nullptr) {
      *error_detail = "runtime profile output pointer must be non-null";
    }
    return false;
  }

  const std::filesystem::path file_path = std::filesystem::absolute(std::filesystem::path(config_path));
  std::ifstream in(file_path, std::ios::binary);
  if (!in.is_open()) {
    if (error_detail != nullptr) {
      *error_detail = "failed to open runtime profile file: " + file_path.string();
    }
    return false;
  }

  *profile = RuntimeProfileData{};

  static const std::unordered_set<std::string> kAllowedKeys = {
      "role",
      "target_host",
      "target_port",
      "rendezvous_url",
      "session_code",
      "run_seconds",
      "signal_timeout_seconds",
      "signal_dir",
      "signal_transport",
      "signal_passphrase",
      "dht_bootstrap",
      "dht_bootstrap_nodes",
      "network_bind_address",
      "ice_udp_port",
      "dht_listen_port",
      "dht_poll_interval_ms",
      "dht_publish_retry_ms",
      "enable_port_mapping",
      "enable_ipv6_candidates",
      "allow_remote_input",
      "allow_remote_agent",
      "agent_project_manifest",
      "helper_role",
      "ice_server",
      "ice_servers",
      "enable_ice_tcp",
      "stream_smoke",
      "stream_require_capture",
      "stream_preview_width",
      "stream_video_max_width",
      "cli",
  };

  std::string line;
  std::size_t line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    const std::string trimmed = trim_copy(line);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
      continue;
    }

    const std::size_t equals_pos = trimmed.find('=');
    if (equals_pos == std::string::npos) {
      if (error_detail != nullptr) {
        *error_detail = "invalid runtime profile line " + std::to_string(line_number) + ": expected key=value";
      }
      return false;
    }

    const std::string key = to_lower_ascii(trim_copy(std::string_view(trimmed).substr(0, equals_pos)));
    const std::string value = trim_copy(std::string_view(trimmed).substr(equals_pos + 1));

    if (key.empty()) {
      if (error_detail != nullptr) {
        *error_detail = "invalid runtime profile line " + std::to_string(line_number) + ": empty key";
      }
      return false;
    }

    if (kAllowedKeys.find(key) == kAllowedKeys.end()) {
      if (error_detail != nullptr) {
        *error_detail = "invalid runtime profile key at line " + std::to_string(line_number) + ": " + key;
      }
      return false;
    }

    if (key == "role") {
      profile->role = value;
      continue;
    }
    if (key == "target_host") {
      profile->target_host = value;
      continue;
    }
    if (key == "target_port") {
      std::uint16_t parsed = 0;
      if (!parse_uint16(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile target_port at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->target_port = parsed;
      continue;
    }
    if (key == "rendezvous_url") {
      profile->rendezvous_url = value;
      continue;
    }
    if (key == "session_code") {
      profile->session_code = value;
      continue;
    }
    if (key == "run_seconds") {
      std::uint32_t parsed = 0;
      if (!parse_uint32(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile run_seconds at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->run_seconds = parsed;
      continue;
    }
    if (key == "signal_timeout_seconds") {
      std::uint32_t parsed = 0;
      if (!parse_uint32(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile signal_timeout_seconds at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->signal_timeout_seconds = parsed;
      continue;
    }
    if (key == "signal_dir") {
      profile->signal_dir = value;
      continue;
    }
    if (key == "signal_transport") {
      profile->signal_transport = value;
      continue;
    }
    if (key == "signal_passphrase") {
      profile->signal_passphrase = value;
      continue;
    }
    if (key == "dht_bootstrap") {
      if (!value.empty()) {
        profile->dht_bootstrap_nodes.push_back(value);
      }
      continue;
    }
    if (key == "dht_bootstrap_nodes") {
      std::stringstream nodes(value);
      std::string token;
      while (std::getline(nodes, token, ',')) {
        const std::string trimmed_token = trim_copy(token);
        if (!trimmed_token.empty()) {
          profile->dht_bootstrap_nodes.push_back(trimmed_token);
        }
      }
      continue;
    }
    if (key == "dht_listen_port") {
      std::uint16_t parsed = 0;
      if (!parse_uint16(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile dht_listen_port at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->dht_listen_port = parsed;
      continue;
    }
    if (key == "network_bind_address") {
      profile->network_bind_address = value;
      continue;
    }
    if (key == "dht_poll_interval_ms") {
      std::uint32_t parsed = 0;
      if (!parse_uint32(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile dht_poll_interval_ms at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->dht_poll_interval_ms = parsed;
      continue;
    }
    if (key == "dht_publish_retry_ms") {
      std::uint32_t parsed = 0;
      if (!parse_uint32(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile dht_publish_retry_ms at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->dht_publish_retry_ms = parsed;
      continue;
    }
    if (key == "enable_port_mapping") {
      bool parsed = false;
      if (!parse_bool_token(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile enable_port_mapping at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->enable_port_mapping = parsed;
      continue;
    }
    if (key == "enable_ipv6_candidates") {
      bool parsed = false;
      if (!parse_bool_token(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile enable_ipv6_candidates at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->enable_ipv6_candidates = parsed;
      continue;
    }
    if (key == "ice_udp_port") {
      std::uint16_t parsed = 0;
      if (!parse_uint16(value, &parsed) || parsed == 0) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile ice_udp_port at line "
              + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->ice_udp_port = parsed;
      continue;
    }
    if (key == "allow_remote_input") {
      bool parsed = false;
      if (!parse_bool_token(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile allow_remote_input at line "
              + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->allow_remote_input = parsed;
      continue;
    }
    if (key == "allow_remote_agent") {
      bool parsed = false;
      if (!parse_bool_token(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile allow_remote_agent at line "
              + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->allow_remote_agent = parsed;
      continue;
    }
    if (key == "agent_project_manifest") {
      if (value.empty()) {
        if (error_detail != nullptr) {
          *error_detail = "runtime profile agent_project_manifest must not be empty at line "
              + std::to_string(line_number);
        }
        return false;
      }
      profile->agent_project_manifest = value;
      continue;
    }
    if (key == "helper_role") {
      const std::string lowered = to_lower_ascii(value);
      if (lowered != "off" && lowered != "client" && lowered != "lan-helper" && lowered != "relay") {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile helper_role at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->helper_role = lowered;
      continue;
    }
    if (key == "ice_server") {
      if (!value.empty()) {
        profile->ice_servers.push_back(value);
      }
      continue;
    }
    if (key == "ice_servers") {
      std::stringstream servers(value);
      std::string token;
      while (std::getline(servers, token, ',')) {
        const std::string trimmed_token = trim_copy(token);
        if (!trimmed_token.empty()) {
          profile->ice_servers.push_back(trimmed_token);
        }
      }
      continue;
    }
    if (key == "enable_ice_tcp") {
      bool parsed = false;
      if (!parse_bool_token(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile enable_ice_tcp at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->enable_ice_tcp = parsed;
      continue;
    }
    if (key == "stream_smoke") {
      bool parsed = false;
      if (!parse_bool_token(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile stream_smoke at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->stream_smoke = parsed;
      continue;
    }
    if (key == "stream_require_capture") {
      bool parsed = false;
      if (!parse_bool_token(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile stream_require_capture at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->stream_require_capture = parsed;
      continue;
    }
    if (key == "stream_preview_width") {
      std::uint32_t parsed = 0;
      if (!parse_uint32(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile stream_preview_width at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->stream_preview_width = parsed;
      continue;
    }
    if (key == "stream_video_max_width") {
      std::uint32_t parsed = 0;
      if (!parse_uint32(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile stream_video_max_width at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->stream_video_max_width = parsed;
      continue;
    }
    if (key == "cli") {
      bool parsed = false;
      if (!parse_bool_token(value, &parsed)) {
        if (error_detail != nullptr) {
          *error_detail = "invalid runtime profile cli at line " + std::to_string(line_number) + ": " + value;
        }
        return false;
      }
      profile->cli = parsed;
      continue;
    }
  }

  if (!in.good() && !in.eof()) {
    if (error_detail != nullptr) {
      *error_detail = "failed while reading runtime profile file: " + file_path.string();
    }
    return false;
  }

  if (error_detail != nullptr) {
    error_detail->clear();
  }
  return true;
}

}  // namespace redclaw::helper
