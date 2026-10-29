#include "ui/gui_startup_options.h"
#include <QDir>
#include <QRegularExpression>

namespace redclaw::ui {
bool parse_gui_auto_start_options(
    const QStringList& args,
    GuiAutoStartOptions* options,
    QString* error_detail, GuiStartupOrigin origin) {
  if (options == nullptr) {
    return false;
  }

  auto require_value = [&](int* index, QString* value) {
    if (*index + 1 >= args.size()) {
      if (error_detail != nullptr) {
        *error_detail = QString("missing value for %1").arg(args[*index]);
      }
      return false;
    }
    ++(*index);
    *value = args[*index];
    return true;
  };

  for (int i = 1; i < args.size(); ++i) {
    const QString arg = args[i];
    QString value;
    if (arg == "--gui-auto-start") {
      options->auto_start = true;
      continue;
    }
    if (arg == "--gui-persist-host-wait") {
      options->persist_host_wait = true;
      continue;
    }
    if (arg == "--connection-credential-file") {
      if (!require_value(&i, &value)) return false;
      options->connection_credential_file = value; continue;
    }
    if (arg == "--gui-role") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->role = value.trimmed().toLower();
      continue;
    }
    if (arg == "--signal-transport") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->transport = value.trimmed();
      continue;
    }
    if (arg == "--target-host") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->target_host = value.trimmed();
      continue;
    }
    if (arg == "--target-port") {
      if (!require_value(&i, &value)) {
        return false;
      }
      bool ok = false;
      const uint parsed = value.toUInt(&ok);
      if (!ok || parsed > 65535U) {
        if (error_detail != nullptr) {
          *error_detail = QString("invalid --target-port value: %1").arg(value);
        }
        return false;
      }
      options->target_port = static_cast<quint16>(parsed);
      continue;
    }
    if (arg == "--rendezvous-url") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->rendezvous_url = value.trimmed();
      continue;
    }
    if (arg == "--session-code") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->session_code = value.trimmed().toUpper();
      continue;
    }
    if (arg == "--signal-dir") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->signal_dir = value.trimmed();
      continue;
    }
    if (arg == "--signal-passphrase") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->signal_passphrase = value;
      continue;
    }
    if (arg == "--run-seconds") {
      if (!require_value(&i, &value)) {
        return false;
      }
      bool ok = false;
      options->run_seconds = value.toUInt(&ok);
      if (!ok) {
        if (error_detail != nullptr) {
          *error_detail = QString("invalid --run-seconds value: %1").arg(value);
        }
        return false;
      }
      continue;
    }
    if (arg == "--signal-timeout-seconds") {
      if (!require_value(&i, &value)) {
        return false;
      }
      bool ok = false;
      options->signal_timeout_seconds = value.toUInt(&ok);
      if (!ok) {
        if (error_detail != nullptr) {
          *error_detail = QString("invalid --signal-timeout-seconds value: %1").arg(value);
        }
        return false;
      }
      continue;
    }
    if (arg == "--stream-preview-width") {
      if (!require_value(&i, &value)) {
        return false;
      }
      bool ok = false;
      options->stream_preview_width = value.toUInt(&ok);
      if (!ok) {
        if (error_detail != nullptr) {
          *error_detail = QString("invalid --stream-preview-width value: %1").arg(value);
        }
        return false;
      }
      continue;
    }
    if (arg == "--stream-video-max-width") {
      if (!require_value(&i, &value)) {
        return false;
      }
      bool ok = false;
      options->stream_video_max_width = value.toUInt(&ok);
      if (!ok) {
        if (error_detail != nullptr) {
          *error_detail = QString("invalid --stream-video-max-width value: %1").arg(value);
        }
        return false;
      }
      continue;
    }
    if (arg == "--stream-smoke") {
      options->stream_smoke = true;
      continue;
    }
    if (arg == "--stream-require-capture") {
      options->stream_smoke = true;
      options->stream_require_capture = true;
      continue;
    }
    if (arg == "--stream-qa-drop-one-media-fragment") {
      options->stream_qa_drop_one_media_fragment = true;
      continue;
    }
    if (arg == "--stream-qa-incomplete-feedback-class") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->stream_qa_incomplete_feedback_class = value.trimmed().toLower();
      continue;
    }
    if (arg == "--stream-qa-force-required-channel-close") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->stream_qa_force_required_channel_close = value.trimmed().toLower();
      continue;
    }
    if (arg == "--stream-qa-native-size") {
      options->stream_qa_native_size = true;
      continue;
    }
    if (arg == "--input-diagnostics") {
      options->input_diagnostics = true;
      continue;
    }
    if (arg == "--agent-qa-fixture-provider") {
      options->agent_qa_fixture_provider = true;
      continue;
    }
    if (arg == "--agent-qa-force-channel-close-after-event") {
      options->agent_qa_force_channel_close_after_event = true;
      continue;
    }
    if (arg == "--enable-ice-tcp") {
      options->enable_ice_tcp = true;
      continue;
    }
    if (arg == "--enable-port-mapping") {
      options->enable_port_mapping = true;
      continue;
    }
    if (arg == "--dht-listen-port") {
      if (!require_value(&i, &value)) {
        return false;
      }
      bool ok = false;
      const uint parsed = value.toUInt(&ok);
      if (!ok || parsed > 65535U) {
        if (error_detail != nullptr) {
          *error_detail = QString("invalid --dht-listen-port value: %1").arg(value);
        }
        return false;
      }
      options->dht_listen_port = static_cast<quint16>(parsed);
      continue;
    }
    if (arg == "--ice-udp-port") {
      if (!require_value(&i, &value)) {
        return false;
      }
      bool ok = false;
      const uint parsed = value.toUInt(&ok);
      if (!ok || parsed == 0 || parsed > 65535U) {
        if (error_detail != nullptr) {
          *error_detail = QString("invalid --ice-udp-port value: %1").arg(value);
        }
        return false;
      }
      options->ice_udp_port = static_cast<quint16>(parsed);
      options->ice_udp_port_explicit = true;
      continue;
    }
    if (arg == "--ice-server") {
      if (!require_value(&i, &value)) {
        return false;
      }
      const QString trimmed = value.trimmed();
      if (!trimmed.isEmpty()) {
        options->ice_servers.push_back(trimmed);
      }
      continue;
    }
    if (arg == "--network-bind-address") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->network_bind_address = value.trimmed();
      continue;
    }
    if (arg == "--network-bind-ipv6-address") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->network_bind_ipv6_address = value.trimmed();
      continue;
    }
    if (arg == "--ice-server-file") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->ice_server_file = value.trimmed();
      continue;
    }
    if (arg == "--log-dir") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->log_dir = value.trimmed();
      continue;
    }
    if (arg == "--run-id") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->run_id = value.trimmed();
      continue;
    }
    if (arg == "--enable-debug-control") {
      options->enable_debug_control = true;
      continue;
    }
    if (arg == "--enable-agent-control") {
      options->enable_agent_control = true;
      continue;
    }
    if (arg == "--enable-workspace-control") {
      options->enable_workspace_control = true;
      continue;
    }
    if (arg == "--workspace-control-name") {
      if (!require_value(&i, &value)) return false;
      options->workspace_control_name = value.trimmed();
      continue;
    }
    if (arg == "--allow-remote-agent") {
      options->allow_remote_agent = true;
      continue;
    }
    if (arg == "--agent-project-root") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->agent_project_roots.push_back(QDir::cleanPath(value.trimmed()));
      continue;
    }
    if (arg == "--debug-control-name") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->debug_control_name = value.trimmed();
      continue;
    }
    if (arg == "--agent-control-name") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->agent_control_name = value.trimmed();
      continue;
    }
    if (arg == "--coordination-git-sha") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->coordination_git_sha = value.trimmed().toLower();
      continue;
    }
    if (arg == "--coordination-journal-path") {
      if (!require_value(&i, &value)) {
        return false;
      }
      options->coordination_journal_path = QDir::cleanPath(value.trimmed());
      continue;
    }
  }

  if (!options->role.isEmpty() && options->role != "host" && options->role != "controller") {
    if (error_detail != nullptr) {
      *error_detail = QString("invalid --gui-role value: %1").arg(options->role);
    }
    return false;
  }
  if (options->debug_control_name.isEmpty() || options->debug_control_name.size() > 128) {
    if (error_detail != nullptr) {
      *error_detail = "invalid --debug-control-name";
    }
    return false;
  }
  if (options->agent_control_name.isEmpty() || options->agent_control_name.size() > 128) {
    if (error_detail != nullptr) {
      *error_detail = "invalid --agent-control-name";
    }
    return false;
  }
  if (!options->coordination_git_sha.isEmpty()
      && !QRegularExpression("^[0-9a-f]{7,64}$")
              .match(options->coordination_git_sha).hasMatch()) {
    if (error_detail != nullptr) {
      *error_detail = "invalid --coordination-git-sha";
    }
    return false;
  }
  if (options->coordination_journal_path.size() > 1024) {
    if (error_detail != nullptr) {
      *error_detail = "invalid --coordination-journal-path";
    }
    return false;
  }
  if (options->stream_qa_drop_one_media_fragment
      && (!options->enable_debug_control || options->role != "controller")) {
    if (error_detail != nullptr) {
      *error_detail =
          "--stream-qa-drop-one-media-fragment requires Controller --enable-debug-control";
    }
    return false;
  }
  const bool valid_qa_feedback_class =
      options->stream_qa_incomplete_feedback_class == "fresh"
      || options->stream_qa_incomplete_feedback_class == "delayed"
      || options->stream_qa_incomplete_feedback_class == "expired"
      || options->stream_qa_incomplete_feedback_class == "stale-revision";
  if (!valid_qa_feedback_class) {
    if (error_detail != nullptr) {
      *error_detail = "invalid --stream-qa-incomplete-feedback-class";
    }
    return false;
  }
  if (options->stream_qa_incomplete_feedback_class != "fresh"
      && !options->stream_qa_drop_one_media_fragment) {
    if (error_detail != nullptr) {
      *error_detail =
          "non-fresh --stream-qa-incomplete-feedback-class requires fragment-loss QA";
    }
    return false;
  }
  if (!options->stream_qa_force_required_channel_close.isEmpty()) {
    if (!options->enable_debug_control || options->role != "controller") {
      if (error_detail != nullptr) {
        *error_detail =
            "--stream-qa-force-required-channel-close requires Controller --enable-debug-control";
      }
      return false;
    }
    if (options->transport != "dht") {
      if (error_detail != nullptr) {
        *error_detail = "--stream-qa-force-required-channel-close requires DHT signaling";
      }
      return false;
    }
    if (options->stream_qa_force_required_channel_close != "media"
        && options->stream_qa_force_required_channel_close != "control") {
      if (error_detail != nullptr) {
        *error_detail = "invalid --stream-qa-force-required-channel-close";
      }
      return false;
    }
  }
  if (options->input_diagnostics) {
#ifdef NDEBUG
    if (error_detail) *error_detail = "--input-diagnostics is Debug-only";
    return false;
#else
    if (!options->enable_debug_control || !options->stream_smoke || options->agent_qa_fixture_provider) {
      if (error_detail) *error_detail = "Input diagnostics require Debug control, stream smoke and a real Agent provider";
      return false;
    }
#endif
  }
  if (options->agent_qa_fixture_provider) {
#ifdef NDEBUG
    if (error_detail) *error_detail = "--agent-qa-fixture-provider is Debug-only";
    return false;
#else
    if (!options->enable_debug_control || !options->stream_smoke) {
      if (error_detail) *error_detail = "Agent fixture requires Debug control and stream smoke";
      return false;
    }
#endif
  }
  if (options->agent_qa_force_channel_close_after_event) {
#ifdef NDEBUG
    if (error_detail != nullptr) {
      *error_detail = "--agent-qa-force-channel-close-after-event is Debug-only";
    }
    return false;
#else
    if (!options->enable_debug_control
        || options->transport != "dht" || !options->stream_smoke) {
      if (error_detail != nullptr) {
        *error_detail =
            "--agent-qa-force-channel-close-after-event requires Debug DHT stream smoke";
      }
      return false;
    }
#endif
  }
  if (options->allow_remote_agent
      && options->agent_project_roots.isEmpty()
      && origin != GuiStartupOrigin::kValidatedMaintenance) {
    if (error_detail != nullptr) {
      *error_detail =
          "--allow-remote-agent GUI automation requires --agent-project-root";
    }
    return false;
  }

  return true;
}

} // namespace redclaw::ui
