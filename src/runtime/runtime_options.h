#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace redclaw::runtime {
#ifdef NDEBUG
inline constexpr bool kWorkspaceControlEnabledByDefault = false;
#else
inline constexpr bool kWorkspaceControlEnabledByDefault = true;
#endif

// Startup configuration only; owns no session or thread state.
// Preserve profile/CLI precedence and GUI pass-through flags.
enum class RuntimeRole {
    kNone,
    kHost,
    kController,
};

struct RuntimeOptions {
    RuntimeRole role = RuntimeRole::kNone;
    std::string role_name;
    std::string target_host;
    std::uint16_t target_port = 0;
    std::string rendezvous_url;
    std::string session_code;
    std::uint32_t run_seconds = 0;
    std::uint32_t signal_timeout_seconds = 120;
    std::string signal_dir = "runtime-signaling";
    std::string signal_transport = "file";
    std::string signal_passphrase;
    std::vector<std::string> dht_bootstrap_nodes;
    std::string network_bind_address;
    std::string network_bind_ipv6_address;
    std::uint16_t ice_udp_port = 55000;
    std::uint16_t dht_listen_port = 0;
    std::uint32_t dht_poll_interval_ms = 1000;
    std::uint32_t dht_publish_retry_ms = 5000;
    bool enable_port_mapping = false;
    bool enable_ipv6_candidates = true;
    bool allow_remote_diagnostics = false;
    bool allow_remote_input = false;
    bool allow_remote_agent = false;
    std::string agent_project_manifest;
    std::string helper_role = "off";
    std::vector<std::string> ice_servers;
    std::string ice_server_file;
    bool enable_ice_tcp = false;
    bool stream_smoke = false;
    bool stream_require_capture = false;
    bool stream_qa_native_size = false;
    bool gui_runtime_stdio = false;
    std::string connection_credential_file;
    bool stream_qa_drop_one_media_fragment = false;
    std::string stream_qa_incomplete_feedback_class = "fresh";
    std::string stream_qa_force_required_channel_close;
    bool agent_qa_force_channel_close_after_event = false;
    bool agent_qa_fixture_provider = false;
    bool input_diagnostics = false;
    std::uint32_t stream_preview_width = 160;
    std::uint32_t stream_video_max_width = 0;
    std::string stream_frame_pipe;
    std::string stream_navigation_pipe;
    std::string log_dir;
    std::string run_id;
    bool enable_debug_control = false;
    std::string debug_control_name = "RedClawDesktop.DebugControl.v1";
    bool enable_agent_control = false;
    bool enable_workspace_control = kWorkspaceControlEnabledByDefault;
    std::string workspace_control_name = "RedClawDesktop.WorkspaceControl.v1";
    std::string agent_control_name = "RedClawDesktop.AgentControl.v1";
    std::string coordination_journal_path;
    std::string coordination_git_sha;
    bool force_cli = false;
    bool help_requested = false;
};

bool parse_runtime_options(int argc, char** argv, RuntimeOptions* options, std::string* error);
bool validate_runtime_options(const RuntimeOptions& options, std::string* error);
void print_usage();
}  // namespace redclaw::runtime
