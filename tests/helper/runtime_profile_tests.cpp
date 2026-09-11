#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "redclaw/helper/runtime_profile.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
  if (condition) {
    return true;
  }

  std::cerr << "[FAIL] " << message << '\n';
  return false;
}

std::filesystem::path make_temp_profile_path(const std::string& suffix) {
  const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::filesystem::current_path() / "redclaw-runtime-profile-test-scratch"
      / ("redclaw-runtime-profile-" + std::to_string(stamp) + "-" + suffix + ".conf");
}

bool write_file(const std::filesystem::path& path, const std::string& content, std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    *error = "failed to create test profile directory: " + ec.message();
    return false;
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    *error = "failed to open test profile: " + path.string();
    return false;
  }

  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!out.good()) {
    *error = "failed to write test profile: " + path.string();
    return false;
  }

  return true;
}

bool test_runtime_profile_parses_valid_content() {
  const auto profile_path = make_temp_profile_path("valid");
  std::string io_error;
  if (!write_file(
        profile_path,
        "role=controller\n"
        "target_host=10.0.0.2\n"
        "target_port=45909\n"
        "rendezvous_url=https://rendezvous.example.com/api\n"
        "session_code=A1B2C3D4\n"
        "run_seconds=10\n"
        "signal_timeout_seconds=45\n"
        "signal_dir=runtime-signaling\n"
        "signal_transport=tcp\n"
        "signal_passphrase=sample-passphrase\n"
        "dht_bootstrap=router.bittorrent.com:6881\n"
        "dht_bootstrap_nodes=dht.transmissionbt.com:6881, dht.libtorrent.org:25401\n"
        "network_bind_address=192.168.0.21\n"
        "ice_udp_port=55001\n"
        "dht_listen_port=45911\n"
        "dht_poll_interval_ms=1000\n"
        "dht_publish_retry_ms=5000\n"
        "enable_port_mapping=true\n"
        "enable_ipv6_candidates=false\n"
        "allow_remote_input=true\n"
        "allow_remote_agent=true\n"
        "agent_project_manifest=runtime-agent-projects.conf\n"
        "helper_role=lan-helper\n"
        "ice_server=stun:stun.l.google.com:19302\n"
        "ice_servers=stun:example.com:3478, turn:relay.example.com:3478\n"
        "enable_ice_tcp=true\n"
        "stream_smoke=true\n"
        "stream_require_capture=true\n"
        "stream_preview_width=160\n"
        "stream_video_max_width=1280\n"
        "cli=1\n",
        &io_error)) {
    std::cerr << "[FAIL] " << io_error << '\n';
    return false;
  }

  redclaw::helper::RuntimeProfileData profile;
  std::string error;
  const bool ok = redclaw::helper::load_runtime_profile_file(profile_path.string(), &profile, &error);

  std::error_code ec;
  std::filesystem::remove(profile_path, ec);

  bool result = true;
  result = expect_true(ok, "valid profile should parse") && result;
  result = expect_true(error.empty(), "valid profile should not produce error") && result;
  result = expect_true(profile.role.has_value() && profile.role.value() == "controller", "role should be parsed") && result;
  result = expect_true(profile.target_host.has_value() && profile.target_host.value() == "10.0.0.2", "target_host should be parsed") && result;
  result = expect_true(profile.target_port.has_value() && profile.target_port.value() == 45909, "target_port should be parsed") && result;
  result = expect_true(profile.rendezvous_url.has_value() && profile.rendezvous_url.value() == "https://rendezvous.example.com/api", "rendezvous_url should be parsed") && result;
  result = expect_true(profile.session_code.has_value() && profile.session_code.value() == "A1B2C3D4", "session_code should be parsed") && result;
  result = expect_true(profile.run_seconds.has_value() && profile.run_seconds.value() == 10, "run_seconds should be parsed") && result;
  result = expect_true(profile.signal_timeout_seconds.has_value() && profile.signal_timeout_seconds.value() == 45, "signal_timeout_seconds should be parsed") && result;
  result = expect_true(profile.signal_transport.has_value() && profile.signal_transport.value() == "tcp", "signal_transport should be parsed") && result;
  result = expect_true(profile.signal_passphrase.has_value() && profile.signal_passphrase.value() == "sample-passphrase", "signal_passphrase should be parsed") && result;
  result = expect_true(profile.dht_bootstrap_nodes.size() == 3, "dht bootstrap nodes should be parsed") && result;
  result = expect_true(profile.network_bind_address.has_value() && profile.network_bind_address.value() == "192.168.0.21", "network_bind_address should be parsed") && result;
  result = expect_true(profile.ice_udp_port.has_value() && profile.ice_udp_port.value() == 55001, "ice_udp_port should be parsed") && result;
  result = expect_true(profile.dht_listen_port.has_value() && profile.dht_listen_port.value() == 45911, "dht_listen_port should be parsed") && result;
  result = expect_true(profile.dht_poll_interval_ms.has_value() && profile.dht_poll_interval_ms.value() == 1000, "dht_poll_interval_ms should be parsed") && result;
  result = expect_true(profile.dht_publish_retry_ms.has_value() && profile.dht_publish_retry_ms.value() == 5000, "dht_publish_retry_ms should be parsed") && result;
  result = expect_true(profile.enable_port_mapping.has_value() && profile.enable_port_mapping.value(), "enable_port_mapping should be true") && result;
  result = expect_true(profile.enable_ipv6_candidates.has_value() && !profile.enable_ipv6_candidates.value(), "enable_ipv6_candidates should be false") && result;
  result = expect_true(profile.allow_remote_input.has_value() && profile.allow_remote_input.value(), "allow_remote_input should be true") && result;
  result = expect_true(profile.allow_remote_agent.has_value() && profile.allow_remote_agent.value(), "allow_remote_agent should be true") && result;
  result = expect_true(profile.agent_project_manifest.has_value()
      && profile.agent_project_manifest.value() == "runtime-agent-projects.conf",
      "agent_project_manifest should be parsed") && result;
  result = expect_true(profile.helper_role.has_value() && profile.helper_role.value() == "lan-helper", "helper_role should be parsed") && result;
  result = expect_true(profile.enable_ice_tcp.has_value() && profile.enable_ice_tcp.value(), "enable_ice_tcp should be true") && result;
  result = expect_true(profile.stream_smoke.has_value() && profile.stream_smoke.value(), "stream_smoke should be true") && result;
  result = expect_true(profile.stream_require_capture.has_value() && profile.stream_require_capture.value(), "stream_require_capture should be true") && result;
  result = expect_true(profile.stream_preview_width.has_value() && profile.stream_preview_width.value() == 160, "stream_preview_width should be parsed") && result;
  result = expect_true(profile.stream_video_max_width.has_value() && profile.stream_video_max_width.value() == 1280, "stream_video_max_width should be parsed") && result;
  result = expect_true(profile.cli.has_value() && profile.cli.value(), "cli should be true") && result;
  result = expect_true(profile.ice_servers.size() == 3, "ice_server and ice_servers should merge entries") && result;
  return result;
}

bool test_runtime_profile_rejects_unknown_key() {
  const auto profile_path = make_temp_profile_path("unknown-key");
  std::string io_error;
  if (!write_file(profile_path, "unexpected_key=value\n", &io_error)) {
    std::cerr << "[FAIL] " << io_error << '\n';
    return false;
  }

  redclaw::helper::RuntimeProfileData profile;
  std::string error;
  const bool ok = redclaw::helper::load_runtime_profile_file(profile_path.string(), &profile, &error);

  std::error_code ec;
  std::filesystem::remove(profile_path, ec);

  return expect_true(!ok, "profile with unknown key should fail")
      && expect_true(error.find("invalid runtime profile key") != std::string::npos, "unknown key should produce detailed error");
}

bool test_runtime_profile_rejects_removed_dht_listen_address() {
  const auto profile_path = make_temp_profile_path("removed-dht-listen-address");
  std::string io_error;
  if (!write_file(profile_path, "dht_listen_address=192.168.0.21\n", &io_error)) {
    std::cerr << "[FAIL] " << io_error << '\n';
    return false;
  }

  redclaw::helper::RuntimeProfileData profile;
  std::string error;
  const bool ok = redclaw::helper::load_runtime_profile_file(profile_path.string(), &profile, &error);

  std::error_code ec;
  std::filesystem::remove(profile_path, ec);

  return expect_true(!ok, "removed dht_listen_address key should fail")
      && expect_true(
          error.find("invalid runtime profile key") != std::string::npos,
          "removed dht_listen_address should not remain as a compatibility path");
}

bool test_runtime_profile_rejects_invalid_bool() {
  const auto profile_path = make_temp_profile_path("invalid-bool");
  std::string io_error;
  if (!write_file(profile_path, "enable_ice_tcp=maybe\n", &io_error)) {
    std::cerr << "[FAIL] " << io_error << '\n';
    return false;
  }

  redclaw::helper::RuntimeProfileData profile;
  std::string error;
  const bool ok = redclaw::helper::load_runtime_profile_file(profile_path.string(), &profile, &error);

  std::error_code ec;
  std::filesystem::remove(profile_path, ec);

  return expect_true(!ok, "invalid bool should fail")
      && expect_true(error.find("enable_ice_tcp") != std::string::npos, "invalid bool error should mention key");
}

bool test_runtime_profile_rejects_invalid_ice_udp_port() {
  const auto profile_path = make_temp_profile_path("invalid-ice-udp-port");
  std::string io_error;
  if (!write_file(profile_path, "ice_udp_port=0\n", &io_error)) {
    std::cerr << "[FAIL] " << io_error << '\n';
    return false;
  }

  redclaw::helper::RuntimeProfileData profile;
  std::string error;
  const bool ok = redclaw::helper::load_runtime_profile_file(profile_path.string(), &profile, &error);

  std::error_code ec;
  std::filesystem::remove(profile_path, ec);

  return expect_true(!ok, "zero ICE UDP port should fail")
      && expect_true(error.find("ice_udp_port") != std::string::npos, "invalid ICE UDP port error should mention key");
}

bool test_runtime_profile_rejects_invalid_helper_role() {
  const auto profile_path = make_temp_profile_path("invalid-helper-role");
  std::string io_error;
  if (!write_file(profile_path, "helper_role=router\n", &io_error)) {
    std::cerr << "[FAIL] " << io_error << '\n';
    return false;
  }

  redclaw::helper::RuntimeProfileData profile;
  std::string error;
  const bool ok = redclaw::helper::load_runtime_profile_file(profile_path.string(), &profile, &error);

  std::error_code ec;
  std::filesystem::remove(profile_path, ec);

  return expect_true(!ok, "invalid helper_role should fail")
      && expect_true(error.find("helper_role") != std::string::npos, "invalid helper_role error should mention key");
}

}  // namespace

int main() {
  bool ok = true;
  ok = test_runtime_profile_parses_valid_content() && ok;
  ok = test_runtime_profile_rejects_unknown_key() && ok;
  ok = test_runtime_profile_rejects_removed_dht_listen_address() && ok;
  ok = test_runtime_profile_rejects_invalid_bool() && ok;
  ok = test_runtime_profile_rejects_invalid_ice_udp_port() && ok;
  ok = test_runtime_profile_rejects_invalid_helper_role() && ok;

  if (!ok) {
    return 1;
  }

  std::cout << "[PASS] redclaw_helper_runtime_profile_tests" << '\n';
  return 0;
}
