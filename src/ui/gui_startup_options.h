#pragma once
#include "runtime/runtime_options.h"
#include <QStringList>

namespace redclaw::ui {
// Maintenance is selected only after the private Host snapshot is validated.
// Its existing project registration and exact runtime authorization are retained;
// fresh GUI automation still has to supply its own project roots.
enum class GuiStartupOrigin { kCommandLine, kValidatedMaintenance };

struct GuiAutoStartOptions {
  bool auto_start = false;
  bool persist_host_wait = false;
  QString role;
  QString transport;
  QString target_host;
  QString rendezvous_url;
  QString session_code;
  QString signal_dir;
  QString signal_passphrase;
  QString connection_credential_file;
  QStringList ice_servers;
  QString ice_server_file;
  QString network_bind_address;
  QString network_bind_ipv6_address;
  QString log_dir;
  QString run_id;
  QString agent_control_name = "RedClawDesktop.AgentControl.v1";
  QString workspace_control_name = "RedClawDesktop.WorkspaceControl.v1";
  bool enable_workspace_control = runtime::kWorkspaceControlEnabledByDefault;
  QString coordination_journal_path;
  QString coordination_git_sha;
  QString debug_control_name = "RedClawDesktop.DebugControl.v1";
  quint16 target_port = 0;
  quint16 ice_udp_port = 55000;
  bool ice_udp_port_explicit = false;
  quint32 run_seconds = 0;
  quint32 signal_timeout_seconds = 120;
  quint32 stream_preview_width = 160;
  quint32 stream_video_max_width = 0;
  bool stream_qa_native_size = false;
  bool stream_smoke = false;
  bool stream_require_capture = false;
  bool stream_qa_drop_one_media_fragment = false;
  QString stream_qa_incomplete_feedback_class = "fresh";
  QString stream_qa_force_required_channel_close;
  bool agent_qa_force_channel_close_after_event = false;
  bool agent_qa_fixture_provider = false;
  bool input_diagnostics = false;
  bool enable_ice_tcp = false;
  bool enable_port_mapping = false;
  bool enable_debug_control = false;
  bool enable_agent_control = false;
  bool allow_remote_agent = false;
  QStringList agent_project_roots;
  quint16 dht_listen_port = 0;
};

bool parse_gui_auto_start_options(
    const QStringList& args, GuiAutoStartOptions* options, QString* error_detail,
    GuiStartupOrigin origin = GuiStartupOrigin::kCommandLine);
} // namespace redclaw::ui
