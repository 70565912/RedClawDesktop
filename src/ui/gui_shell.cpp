#include "gui_shell.h"
#include "runtime/runtime_options.h"
#include "playback/capture_playback_state.h"
#include "playback/playback_frame_progress.h"
#if defined(_WIN32) && defined(REDCLAW_ENABLE_QT_GUI)
#include "ui/terminal/terminal_panel.h"
#include "ui/terminal/workspace_pipe_server.h"
#endif

#if defined(REDCLAW_ENABLE_QT_GUI)
#include "ui/file_transfer_panel.h"
#include "ui/transfer_coordinator.h"
#include "ui/workspace_control_server.h"
#ifdef _WIN32
#include "ui/terminal/terminal_coordinator.h"
#endif
#include "ui/runtime_maintenance_context.h"
#include "ui/gui_latency_probe.h"
#include "ui/gui_diagnostic_writer.h"
#include "ui/gui_quit_barrier.h"
#include "ui/runtime_stdio_reader.h"
#include "ui/runtime_control_writer.h"
#include "ui/runtime_log_view.h"
#include "ui/qa_input_probe.h"
#include "ui/input_diagnostic_target.h"
#include "playback/playback_canvas.h"
#include "playback/direct_frame_receiver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <utility>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <QAbstractSocket>
#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QComboBox>
#include <QStandardItemModel>
#include <QDateTime>
#include <QCheckBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFile>
#include <QFrame>
#include <QFormLayout>
#include <QGuiApplication>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QFileInfo>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMainWindow>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPainter>
#include <QPaintEngine>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPropertyAnimation>
#include <QPen>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QSettings>
#include <QSaveFile>
#include <QShowEvent>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QStringList>
#include <QTabBar>
#include <QTabWidget>
#include <QThread>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QProcessEnvironment>
#include <QElapsedTimer>
#include "redclaw/agent/local_agent_pipe.h"
#include <QVBoxLayout>
#include <QWidget>

#if defined(REDCLAW_ENABLE_QT_OPENGL)
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <Windows.h>
#endif

#include "redclaw/render/audio_player.h"
#include "redclaw/diag/diag_module.h"
#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw/protocol/agent_protocol.h"
#include "redclaw/render/render_module.h"
#include "redclaw/helper/direct_frame_shared_memory.h"
#include "redclaw/service/service_module.h"
#include "ui/agent_settings_dialog.h"
#include "redclaw/helper/runtime_profile.h"
#include "ui/agent_conversation_panel.h"
#include "ui/agent_panel_presentation.h"
#include "ui/agent_control_server.h"
#include "ui/connection_entry_page.h"
#include "ui/connection_password_panel.h"
#include "ui/connection_flow_model.h"
#include "ui/connection_progress_page.h"
#include "ui/desktop_navigation_panel.h"
#include "ui/desktop_task_workspace.h"
#include "ui/debug_control_protocol.h"
#include "ui/playback_control_hint_overlay.h"
#include "ui/playback_window_lifecycle.h"
#include "ui/playback_window_geometry_controller.h"
#include "ui/remote_input_capture.h"
#include "ui/stun_server_policy.h"

namespace redclaw::ui {

namespace {

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;
#endif

GuiDiagnosticWriter* g_diagnostic_writer = nullptr;

constexpr int kMinSealedPassphraseLength = 16;
constexpr int kDefaultWindowWidth = 900;
constexpr int kDefaultWindowHeight = 680;
constexpr int kMinimumWindowWidth = 800;
constexpr int kMinimumWindowHeight = 600;

qint64 steady_now_us() {
  return static_cast<qint64>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

class LatestFrameDispatchTarget final : public QObject {
 public:
  LatestFrameDispatchTarget(std::function<void()> callback, QObject* parent)
      : QObject(parent), callback_(std::move(callback)) {}

  void post() {
    QCoreApplication::postEvent(
        this,
        new QEvent(dispatch_event_type()),
        Qt::HighEventPriority);
  }

 protected:
  bool event(QEvent* event) override {
    if (event != nullptr && event->type() == dispatch_event_type()) {
      callback_();
      return true;
    }
    return QObject::event(event);
  }

 private:
  static QEvent::Type dispatch_event_type() {
    static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
    return type;
  }

  std::function<void()> callback_;
};

class ConnectionViewStack final : public QStackedWidget {
 public:
  using QStackedWidget::QStackedWidget;

  [[nodiscard]] QSize sizeHint() const override {
    QWidget* page = currentWidget();
    if (const auto* scroll = qobject_cast<QScrollArea*>(page); scroll != nullptr && scroll->widget() != nullptr) {
      return scroll->widget()->sizeHint();
    }
    return page != nullptr ? page->sizeHint() : QStackedWidget::sizeHint();
  }

  [[nodiscard]] QSize minimumSizeHint() const override {
    return QSize(0, 0);
  }
};


bool is_valid_session_code(const QString& value) {
  if (value.size() != 8) {
    return false;
  }

  for (const QChar ch : value) {
    const ushort u = ch.unicode();
    const bool is_upper = u >= 'A' && u <= 'Z';
    const bool is_digit = u >= '0' && u <= '9';
    if (!is_upper && !is_digit) {
      return false;
    }
  }

  return true;
}

QString now_timestamp() {
  return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
}

QString format_gui_log_line(const QString& line) {
  GuiLatencyScope timing(GuiStage::kLogFormat);
  const QString safe_line = QString::fromStdString(redclaw::diag::redact_log_text(line.toStdString()));
  return QString("[%1] %2").arg(now_timestamp(), safe_line);
}

void write_gui_log_sink(const QString& formatted) {
  GuiLatencyScope timing(GuiStage::kLogSink);
  if (g_diagnostic_writer) g_diagnostic_writer->post_log({"gui", formatted.toUtf8(), false});
}

void append_log(QPlainTextEdit* output, const QString& line) {
  const QString formatted = format_gui_log_line(line);
  GuiLatencyScope timing(GuiStage::kLogWidget);
  output->appendPlainText(formatted);
  timing.finish();
  write_gui_log_sink(formatted);
}

struct NetworkExitChoice {
  QString address;
  QString ipv6_address;
  QString interface_id;
  QString label;
  int priority = 2;
};

QString network_interface_type_label(QNetworkInterface::InterfaceType type) {
  switch (type) {
    case QNetworkInterface::Ethernet:
      return "Ethernet";
    case QNetworkInterface::Wifi:
      return "Wi-Fi";
    case QNetworkInterface::Virtual:
      return "Virtual";
    case QNetworkInterface::Ppp:
    case QNetworkInterface::Slip:
      return "Tunnel";
    default:
      return "Other";
  }
}

QList<NetworkExitChoice> enumerate_network_exit_choices() {
  QList<NetworkExitChoice> choices;
  QStringList seen_addresses;
  for (const QNetworkInterface& network_interface : QNetworkInterface::allInterfaces()) {
    const auto flags = network_interface.flags();
    if (!flags.testFlag(QNetworkInterface::IsUp)
        || !flags.testFlag(QNetworkInterface::IsRunning)
        || flags.testFlag(QNetworkInterface::IsLoopBack)) {
      continue;
    }

    const auto type = network_interface.type();
    const bool physical = type == QNetworkInterface::Ethernet || type == QNetworkInterface::Wifi;
    const bool virtual_or_tunnel = type == QNetworkInterface::Virtual
        || type == QNetworkInterface::Ppp
        || type == QNetworkInterface::Slip;
    const QString interface_name = network_interface.humanReadableName().isEmpty()
        ? network_interface.name()
        : network_interface.humanReadableName();
    QString ipv6_address;
    for (const QNetworkAddressEntry& entry : network_interface.addressEntries()) {
      const QHostAddress address = entry.ip();
      if (address.protocol() == QAbstractSocket::IPv6Protocol
          && !address.isNull()
          && !address.isLoopback()
          && !address.isLinkLocal()) {
        ipv6_address = address.toString();
        break;
      }
    }
    for (const QNetworkAddressEntry& entry : network_interface.addressEntries()) {
      const QHostAddress address = entry.ip();
      if (address.protocol() != QAbstractSocket::IPv4Protocol
          || address.isNull()
          || address.isLoopback()
          || address.isLinkLocal()) {
        continue;
      }
      const QString address_text = address.toString();
      if (seen_addresses.contains(address_text)) {
        continue;
      }
      seen_addresses.push_back(address_text);

      NetworkExitChoice choice;
      choice.address = address_text;
      choice.ipv6_address = ipv6_address;
      choice.interface_id = network_interface.name();
      choice.priority = physical ? 0 : (virtual_or_tunnel ? 1 : 2);
      choice.label = QString("%1 — %2 [%3]")
          .arg(interface_name, address_text, network_interface_type_label(type));
      choices.push_back(std::move(choice));
    }
  }

  std::sort(choices.begin(), choices.end(), [](const auto& left, const auto& right) {
    if (left.priority != right.priority) {
      return left.priority < right.priority;
    }
    return left.label.localeAwareCompare(right.label) < 0;
  });
  return choices;
}

void populate_network_exit_combo(QComboBox* combo, const QString& preferred_address = {}) {
  combo->clear();
  combo->addItem("Auto (system route)", QString());
  combo->setItemData(0, true, Qt::UserRole + 2);
  combo->setItemData(
      0,
      "Uses the Windows route table. A proxy or tunnel adapter may become the default path.",
      Qt::ToolTipRole);

  int preferred_index = preferred_address.isEmpty() ? 0 : -1;
  for (const NetworkExitChoice& choice : enumerate_network_exit_choices()) {
    const int index = combo->count();
    combo->addItem(choice.label, choice.address);
    combo->setItemData(index, choice.interface_id, Qt::UserRole + 1);
    combo->setItemData(index, true, Qt::UserRole + 2);
    combo->setItemData(index, choice.ipv6_address, Qt::UserRole + 3);
    combo->setItemData(
        index,
        QString("Uses adapter %1. ICE/STUN/TURN and UPnP bind to IPv4 %2; DHT also binds to this adapter's IPv6 when available (%3). Reconnect after changing this setting.")
            .arg(
                choice.interface_id,
                choice.address,
                choice.ipv6_address.isEmpty() ? "none" : choice.ipv6_address),
        Qt::ToolTipRole);
    if (choice.address == preferred_address) {
      preferred_index = index;
    }
  }

  if (!preferred_address.isEmpty() && preferred_index < 0) {
    preferred_index = combo->count();
    combo->addItem(QString("Unavailable — %1").arg(preferred_address), preferred_address);
    combo->setItemData(preferred_index, false, Qt::UserRole + 2);
    combo->setItemData(
        preferred_index,
        "The configured address is not assigned to an active local interface.",
        Qt::ToolTipRole);
  }
  combo->setCurrentIndex(std::max(preferred_index, 0));
}

bool is_network_bind_address_available(const QString& address) {
  if (address.isEmpty()) {
    return true;
  }
  const QList<NetworkExitChoice> choices = enumerate_network_exit_choices();
  return std::any_of(choices.begin(), choices.end(), [&](const auto& choice) {
    return choice.address == address;
  });
}

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
    const QStringList& args,
    GuiAutoStartOptions* options,
    QString* error_detail) {
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
      && options->agent_project_roots.isEmpty()) {
    if (error_detail != nullptr) {
      *error_detail =
          "--allow-remote-agent GUI automation requires --agent-project-root";
    }
    return false;
  }

  return true;
}

QString map_ice_state(QStringView value) {
  bool ok = false;
  const int state = value.toInt(&ok);
  if (!ok) {
    return "unknown";
  }

  switch (state) {
    case 0:
      return "new";
    case 1:
      return "gathering";
    case 2:
      return "connecting";
    case 3:
      return "connected";
    case 4:
      return "disconnected";
    case 5:
      return "failed";
    case 6:
      return "closed";
    default:
      return QString("unknown(%1)").arg(state);
  }
}

QString redact_runtime_args_for_log(const QStringList& args) {
  QStringList redacted;
  redacted.reserve(args.size());

  for (int i = 0; i < args.size(); ++i) {
    const QString& arg = args[i];
    redacted.push_back(arg);
    if (arg == "--signal-passphrase" && i + 1 < args.size()) {
      ++i;
      redacted.push_back("***");
    }
  }

  return QString::fromStdString(redclaw::diag::redact_log_text(redacted.join(' ').toStdString()));
}

QJsonObject debug_runtime_status_json(const redclaw::diag::DebugRuntimeStatus& status);
GuiDiagnosticWriter::StatusFactory debug_runtime_status_work(const redclaw::diag::DebugRuntimeStatus& status) {
  const auto scheduling = gui_latency_probe() ? gui_latency_probe()->snapshot_work() : std::function<QJsonObject()>{};
  const auto writer = g_diagnostic_writer ? g_diagnostic_writer->snapshot() : QJsonObject{};
  return [status, scheduling, writer] {
    auto object = debug_runtime_status_json(status);
    if (scheduling) object.insert("gui_scheduling", scheduling());
    object.insert("diagnostic_writer", writer);
    return object;
  };
}
QJsonObject debug_runtime_status_json(const redclaw::diag::DebugRuntimeStatus& status) {
  QJsonObject object;
  object.insert("schema", QString::fromStdString(status.schema));
  object.insert("run_id", QString::fromStdString(status.run_id));
  object.insert("role", QString::fromStdString(status.role));
  object.insert("phase", QString::fromStdString(status.phase));
  object.insert("negotiation_phase", QString::fromStdString(status.negotiation_phase));
  object.insert("ice_state", QString::fromStdString(status.ice_state));
  object.insert("transport_path", QString::fromStdString(status.transport_path));
  object.insert("stream_health", QString::fromStdString(status.stream_health));
  object.insert("network_bind_address", QString::fromStdString(status.network_bind_address));
  object.insert("local_description_tag", QString::fromStdString(status.local_description_tag));
  object.insert("remote_description_tag", QString::fromStdString(status.remote_description_tag));
  object.insert("last_error", QString::fromStdString(status.last_error));
  object.insert("process_log_path", QString::fromStdString(status.process_log_path));
  object.insert("status_path", QString::fromStdString(status.status_path));
  object.insert("evidence_manifest_path", QString::fromStdString(status.evidence_manifest_path));
  object.insert("app_pid", static_cast<qint64>(status.app_pid));
  object.insert("runtime_pid", static_cast<qint64>(status.runtime_pid));
  object.insert("started_at_unix_ms", static_cast<qint64>(status.started_at_unix_ms));
  object.insert("updated_at_unix_ms", static_cast<qint64>(status.updated_at_unix_ms));
  object.insert("runtime_running", status.runtime_running);
  object.insert("dht_reachable", status.dht_reachable);
  object.insert("answer_acknowledged", status.answer_acknowledged);
  object.insert("remote_description_applied", status.remote_description_applied);
  object.insert("connected", status.connected);
  object.insert("channel_open", status.channel_open);

  QJsonObject ice;
  ice.insert("udp_port", static_cast<int>(status.ice_udp_port));
  ice.insert("port_mapping_status", QString::fromStdString(status.ice_port_mapping_status));
  ice.insert("mapped_internal_port", static_cast<int>(status.ice_mapped_internal_port));
  ice.insert("mapped_external_port", static_cast<int>(status.ice_mapped_external_port));
  ice.insert("mapped_external_ip", QString::fromStdString(status.ice_mapped_external_ip));
  object.insert("ice", ice);

  QJsonObject dht;
  dht.insert("publish_success", static_cast<qint64>(status.dht_publish_success));
  dht.insert("generation_publish_success", static_cast<qint64>(status.dht_generation_publish_success));
  dht.insert("publish_revision", static_cast<qint64>(status.dht_publish_revision));
  dht.insert("publish_expiry", static_cast<qint64>(status.dht_publish_expiry));
  dht.insert("publish_expired_total", static_cast<qint64>(status.dht_publish_expired_total));
  dht.insert("publish_late_results", static_cast<qint64>(status.dht_publish_late_results));
  dht.insert("fetch_hits", static_cast<qint64>(status.dht_fetch_hits));
  dht.insert("generation", static_cast<qint64>(status.dht_generation));
  dht.insert("local_revision", static_cast<qint64>(status.local_revision));
  dht.insert("remote_revision", static_cast<qint64>(status.remote_revision));
  dht.insert("publisher_instance", QString::fromStdString(status.dht_publisher_instance_summary));
  dht.insert("last_host_instance", QString::fromStdString(status.dht_last_host_instance_summary));
  dht.insert("last_adoption_reason", QString::fromStdString(status.dht_last_adoption_reason));
  dht.insert("persistent_offer_adopted_initial_total",
             static_cast<qint64>(status.persistent_offer_adopted_initial_total));
  dht.insert("persistent_offer_adopted_host_restart_total",
             static_cast<qint64>(status.persistent_offer_adopted_host_restart_total));
  dht.insert("persistent_offer_same_instance_rejected_total",
             static_cast<qint64>(status.persistent_offer_same_instance_rejected_total));
  dht.insert("peer_instance_missing_total",
             static_cast<qint64>(status.peer_instance_missing_total));
  dht.insert("duplicate_offer_application_suppressed_total",
             static_cast<qint64>(status.duplicate_offer_application_suppressed_total));
  dht.insert("direct_candidates_published", static_cast<qint64>(status.local_dht_direct_published));
  dht.insert("full_candidates_published", status.full_candidates_published);
  dht.insert("remote_latest_candidates", static_cast<qint64>(status.remote_dht_latest_candidates));
  object.insert("dht", dht);

  QJsonObject candidates;
  candidates.insert("local_host", static_cast<qint64>(status.local_host));
  candidates.insert("local_srflx", static_cast<qint64>(status.local_srflx));
  candidates.insert("local_relay", static_cast<qint64>(status.local_relay));
  candidates.insert("remote_host", static_cast<qint64>(status.remote_host));
  candidates.insert("remote_srflx", static_cast<qint64>(status.remote_srflx));
  candidates.insert("remote_relay", static_cast<qint64>(status.remote_relay));
  object.insert("candidates", candidates);

  const auto diagnostic_json = [](const redclaw::net::TransportDiagnosticEvent& event) {
    return QJsonObject{
        {"sequence", static_cast<qint64>(event.sequence)},
        {"steady_ms", static_cast<qint64>(event.steady_ms)},
        {"peer_generation", static_cast<qint64>(event.peer_generation)},
        {"channel_generation", static_cast<qint64>(event.channel_generation)},
        {"layer", static_cast<int>(event.layer)},
        {"channel", event.channel ? static_cast<int>(*event.channel) : -1},
        {"native_state", event.native_state}, {"failure", event.failure},
        {"pair_id", event.pair_id}, {"local_candidate_type", event.local_candidate_type},
        {"remote_candidate_type", event.remote_candidate_type},
        {"occurrences", static_cast<qint64>(event.occurrences)},
        {"reason", QString::fromStdString(event.reason)}};
  };
  QJsonArray transport_events;
  for (const auto& event : status.transport_diagnostics.recent_events) {
    transport_events.append(diagnostic_json(event));
  }
  QJsonObject transport_diagnostics{{"recent_events", transport_events},
      {"dtls_cause", "unknown"}, {"sctp_cause", "unknown"}};
  QJsonArray candidate_events;
  for (const auto& event : status.transport_diagnostics.candidate_events)
    candidate_events.append(diagnostic_json(event));
  transport_diagnostics.insert("candidate_observations", candidate_events);
  if (status.transport_diagnostics.first_failure) {
    transport_diagnostics.insert("first_failure", diagnostic_json(*status.transport_diagnostics.first_failure));
  }
  object.insert("transport_diagnostics", transport_diagnostics);
  QJsonObject ice_checks{{"peer_generation", static_cast<qint64>(status.transport_diagnostics.ice_check_generation)},
      {"available", status.transport_diagnostics.ice_check_generation != 0},
      {"backend", "libjuice"}, {"ice_tcp_supported", false}};
  for (std::size_t index = 0; index < redclaw::net::kIceCheckKindCount; ++index) {
    ice_checks.insert(QString::fromUtf8(redclaw::net::ice_check_kind_name(
        static_cast<redclaw::net::IceCheckKind>(index)).data()),
        static_cast<qint64>(status.transport_diagnostics.ice_check_totals[index]));
  }
  object.insert("ice_checks", ice_checks);
  QJsonObject pacer;
  pacer.insert("recovery_generation", static_cast<qint64>(status.media_pacer.recovery_generation));
  pacer.insert("dependency_pending_drops", static_cast<qint64>(status.media_pacer.dependency_pending_drops));
  pacer.insert("suppressed_keyframe_requests", static_cast<qint64>(status.media_pacer.suppressed_keyframe_requests));
  pacer.insert("budget_revision", static_cast<qint64>(status.media_pacer.budget_revision));
  pacer.insert("budget_retry_attempt", static_cast<qint64>(status.media_pacer.budget_retry_attempt));
  pacer.insert("next_admission_ms", static_cast<qint64>(status.media_pacer.next_admission_ms));
  pacer.insert("probe_wire_bytes", static_cast<qint64>(status.media_pacer.probe_wire_bytes));
  pacer.insert("frame_budget_rejections", static_cast<qint64>(status.media_pacer.frame_budget_rejections));
  pacer.insert("deadline_drops", static_cast<qint64>(status.media_pacer.deadline_drops));
  pacer.insert("active_depth", static_cast<qint64>(status.media_pacer.active_depth));
  pacer.insert("pending_depth", static_cast<qint64>(status.media_pacer.pending_depth));
  object.insert("media_pacer", pacer);
  object.insert("dht_listener", QJsonObject{
      {"available", status.dht_listener_available}, {"ready", status.dht_listener_ready},
      {"startup_failed", status.dht_listener_startup_failed},
      {"probe_attempts", static_cast<qint64>(status.dht_listener_probe_attempts)}});

  QJsonObject stream;
  stream.insert("captured", static_cast<qint64>(status.captured));
  stream.insert("synthetic", static_cast<qint64>(status.synthetic));
  stream.insert("capture_failures", static_cast<qint64>(status.capture_failures));
  stream.insert("encoded", static_cast<qint64>(status.encoded));
  stream.insert("encode_failures", static_cast<qint64>(status.encode_failures));
  stream.insert("transmitted", static_cast<qint64>(status.transmitted));
  stream.insert("transmit_failures", static_cast<qint64>(status.transmit_failures));
  stream.insert("received", static_cast<qint64>(status.received));
  stream.insert("media_fragments_received", static_cast<qint64>(status.media_fragments_received));
  stream.insert("encoded_frames_reassembled", static_cast<qint64>(status.encoded_frames_reassembled));
  stream.insert("direct_pipe_written", static_cast<qint64>(status.direct_pipe_written));
  stream.insert("decoded", static_cast<qint64>(status.decoded));
  stream.insert("decode_failures", static_cast<qint64>(status.decode_failures));
  stream.insert("rendered", static_cast<qint64>(status.rendered));
  stream.insert("render_failures", static_cast<qint64>(status.render_failures));
  stream.insert("gui_decode_success", static_cast<qint64>(status.gui_decode_success));
  stream.insert("gui_decode_failures", static_cast<qint64>(status.gui_decode_failures));
  stream.insert("gui_presented", static_cast<qint64>(status.gui_presented));
  stream.insert("gui_present_failures", static_cast<qint64>(status.gui_present_failures));
  stream.insert(
      "direct_pipe_writer_busy_drops",
      static_cast<qint64>(status.direct_pipe_writer_busy_drops));
  stream.insert("shared_snapshot_copies", static_cast<qint64>(status.shared_snapshot_copies));
  stream.insert("shared_snapshot_failures", static_cast<qint64>(status.shared_snapshot_failures));
  stream.insert(
      "shared_snapshot_sequence_changes",
      static_cast<qint64>(status.shared_snapshot_sequence_changes));
  stream.insert("shared_snapshot_max_us", static_cast<qint64>(status.shared_snapshot_max_us));
  stream.insert("shared_dependency_catchups", static_cast<qint64>(status.shared_dependency_catchups));
  stream.insert("shared_independent_skips", static_cast<qint64>(status.shared_independent_skips));
  stream.insert("shared_dependency_gaps", static_cast<qint64>(status.shared_dependency_gaps));
  stream.insert("decoder_recovery_state", static_cast<qint64>(status.decoder_recovery_state));
  stream.insert(
      "decoder_recovery_generation",
      static_cast<qint64>(status.decoder_recovery_generation));
  stream.insert(
      "decoder_recovery_breaks_total",
      static_cast<qint64>(status.decoder_recovery_breaks_total));
  stream.insert(
      "decoder_recovery_requests_total",
      static_cast<qint64>(status.decoder_recovery_requests_total));
  stream.insert(
      "decoder_recovery_retries_total",
      static_cast<qint64>(status.decoder_recovery_retries_total));
  stream.insert(
      "decoder_recovery_suppressed_total",
      static_cast<qint64>(status.decoder_recovery_suppressed_total));
  stream.insert(
      "decoder_recovery_displayable_acks_total",
      static_cast<qint64>(status.decoder_recovery_displayable_acks_total));
  stream.insert(
      "decoder_recovery_invalidated_total",
      static_cast<qint64>(status.decoder_recovery_invalidated_total));
  stream.insert("receiver_decode_fps", static_cast<qint64>(status.receiver_decode_fps));
  stream.insert(
      "receiver_decode_pressure_windows",
      static_cast<qint64>(status.receiver_decode_pressure_windows));
  stream.insert(
      "receiver_decode_stable_windows",
      static_cast<qint64>(status.receiver_decode_stable_windows));
  stream.insert(
      "receiver_decode_fps_decrease_total",
      static_cast<qint64>(status.receiver_decode_fps_decrease_total));
  stream.insert(
      "receiver_decode_fps_increase_total",
      static_cast<qint64>(status.receiver_decode_fps_increase_total));
  stream.insert(
      "hardware_decode_runtime_fallbacks",
      static_cast<qint64>(status.hardware_decode_runtime_fallbacks));
  stream.insert("decoder_backend", QString::fromStdString(status.decoder_backend));
  stream.insert("last_decode_error", QString::fromStdString(status.last_decode_error));
  stream.insert(
      "last_hardware_decode_error",
      QString::fromStdString(status.last_hardware_decode_error));
  object.insert("stream", stream);

  QJsonObject agent;
  agent.insert("authorized", status.agent_authorized);
  agent.insert("remote_authorized", status.agent_authorized);
  agent.insert("local_authorized", status.agent_local_authorized);
  agent.insert("local_execution_state", static_cast<qint64>(status.agent_local_execution_state));
  agent.insert("local_execution_task", QString::fromStdString(status.agent_local_execution_task));
  agent.insert("local_execution_queued", static_cast<qint64>(status.agent_local_execution_queued));
  agent.insert("request_queue_depth", static_cast<qint64>(status.agent_request_queue_depth));
  agent.insert("result_queue_depth", static_cast<qint64>(status.agent_result_queue_depth));
  agent.insert("request_queue_peak", static_cast<qint64>(status.agent_request_queue_peak));
  agent.insert("result_queue_peak", static_cast<qint64>(status.agent_result_queue_peak));
  agent.insert("sent_requests", static_cast<qint64>(status.agent_sent_requests));
  agent.insert("sent_results", static_cast<qint64>(status.agent_sent_results));
  agent.insert("received_commands", static_cast<qint64>(status.agent_received_commands));
  agent.insert("received_results", static_cast<qint64>(status.agent_received_results));
  agent.insert("peer_rejected_total", static_cast<qint64>(status.agent_peer_rejected_total));
  agent.insert("peer_stale_total", static_cast<qint64>(status.agent_peer_stale_total));
  agent.insert("send_pump_max_us", static_cast<qint64>(status.agent_send_pump_max_us));
  agent.insert("channel_open", status.agent_channel_open);
  agent.insert("channel_unavailable", status.agent_channel_unavailable);
  agent.insert("channel_open_total", static_cast<qint64>(status.agent_channel_open_total));
  agent.insert("channel_close_total", static_cast<qint64>(status.agent_channel_close_total));
  agent.insert(
      "channel_rebuild_attempt_total",
      static_cast<qint64>(status.agent_channel_rebuild_attempt_total));
  agent.insert(
      "channel_rebuild_success_total",
      static_cast<qint64>(status.agent_channel_rebuild_success_total));
  agent.insert(
      "capability_refresh_total",
      static_cast<qint64>(status.agent_capability_refresh_total));
  agent.insert("task_create_total", static_cast<qint64>(status.agent_task_create_total));
  agent.insert("task_complete_total", static_cast<qint64>(status.agent_task_complete_total));
  agent.insert("task_failed_total", static_cast<qint64>(status.agent_task_failed_total));
  agent.insert(
      "duplicate_task_rejected_total",
      static_cast<qint64>(status.agent_duplicate_task_rejected_total));
  agent.insert("event_total", static_cast<qint64>(status.agent_event_total));
  agent.insert("event_ack_total", static_cast<qint64>(status.agent_event_ack_total));
  agent.insert(
      "replayed_event_total",
      static_cast<qint64>(status.agent_replayed_event_total));
  agent.insert("gap_total", static_cast<qint64>(status.agent_gap_total));
  agent.insert(
      "approval_request_total",
      static_cast<qint64>(status.agent_approval_request_total));
  agent.insert(
      "approval_accept_total",
      static_cast<qint64>(status.agent_approval_accept_total));
  agent.insert(
      "approval_reject_total",
      static_cast<qint64>(status.agent_approval_reject_total));
  agent.insert(
      "approval_timeout_total",
      static_cast<qint64>(status.agent_approval_timeout_total));
  agent.insert("queue_peak", static_cast<qint64>(status.agent_queue_peak));
  agent.insert(
      "cached_event_bytes", static_cast<qint64>(status.agent_cached_event_bytes));
  agent.insert(
      "cached_event_bytes_peak",
      static_cast<qint64>(status.agent_cached_event_bytes_peak));
  agent.insert(
      "outbound_queue_current",
      static_cast<qint64>(status.agent_outbound_queue_current));
  agent.insert(
      "outbound_queue_peak",
      static_cast<qint64>(status.agent_outbound_queue_peak));
  object.insert("agent", agent);
  return object;
}

bool write_json_atomically(const QString& path, const QJsonObject& object, QString* error_detail = nullptr) {
  if (path.isEmpty()) {
    if (error_detail != nullptr) {
      *error_detail = "output path is empty";
    }
    return false;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    if (error_detail != nullptr) {
      *error_detail = file.errorString();
    }
    return false;
  }
  GuiLatencyScope serialize_timing(GuiStage::kStatusSerialize);
  const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Indented);
  serialize_timing.finish();
  GuiLatencyScope commit_timing(GuiStage::kStatusCommit);
  if (file.write(payload) != payload.size() || !file.commit()) {
    if (error_detail != nullptr) {
      *error_detail = file.errorString();
    }
    return false;
  }
  if (error_detail != nullptr) {
    error_detail->clear();
  }
  return true;
}

bool copy_file_atomically(const QString& source_path, const QString& destination_path, QString* error_detail) {
  QFile source(source_path);
  if (!source.open(QIODevice::ReadOnly)) {
    if (error_detail != nullptr) {
      *error_detail = source.errorString();
    }
    return false;
  }
  QSaveFile destination(destination_path);
  if (!destination.open(QIODevice::WriteOnly)) {
    if (error_detail != nullptr) {
      *error_detail = destination.errorString();
    }
    return false;
  }
  while (!source.atEnd()) {
    const QByteArray chunk = source.read(64 * 1024);
    if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
      if (error_detail != nullptr) {
        *error_detail = source.errorString();
      }
      destination.cancelWriting();
      return false;
    }
    if (destination.write(chunk) != chunk.size()) {
      if (error_detail != nullptr) {
        *error_detail = destination.errorString();
      }
      destination.cancelWriting();
      return false;
    }
  }
  if (!destination.commit()) {
    if (error_detail != nullptr) {
      *error_detail = destination.errorString();
    }
    return false;
  }
  if (error_detail != nullptr) {
    error_detail->clear();
  }
  return true;
}

int count_ice_server_file_entries(const QString& path) {
  if (path.isEmpty()) {
    return 0;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return 0;
  }
  int count = 0;
  while (!file.atEnd()) {
    const QByteArray line = file.readLine().trimmed();
    if (!line.isEmpty() && !line.startsWith('#')) {
      ++count;
    }
  }
  return count;
}

QString sha256_file_hex(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) {
    return {};
  }
  return QString::fromLatin1(hash.result().toHex());
}

struct MachineLinkCode {
  QString session_code;
};

QString generate_machine_link_code() {
  constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  QString code;
  code.reserve(8);
  for (int i = 0; i < 8; ++i) {
    const quint32 index = QRandomGenerator::global()->bounded(static_cast<quint32>(sizeof(kAlphabet) - 1));
    code.append(QChar(kAlphabet[index]));
  }
  return code;
}

bool decode_machine_link_code(const QString& code, MachineLinkCode* out, QString* error_detail) {
  if (out == nullptr) {
    if (error_detail != nullptr) {
      *error_detail = "internal error: missing output";
    }
    return false;
  }

  QString compact = code.trimmed();
  compact.remove(QRegularExpression("\\s"));
  compact = compact.toUpper();
  if (!is_valid_session_code(compact)) {
    if (error_detail != nullptr) {
      *error_detail = "machine link code must be 8 chars using A-Z and 0-9";
    }
    return false;
  }

  out->session_code = compact;
  if (error_detail != nullptr) {
    error_detail->clear();
  }
  return true;
}

enum class SealedExchangeState {
  kIdle,
  kGenerated,
  kWaitingRemote,
  kReceived,
  kApplied,
  kFailed,
};

QString sealed_exchange_state_text(SealedExchangeState state, const QString& detail = QString()) {
  QString base;
  switch (state) {
    case SealedExchangeState::kIdle:
      base = "idle";
      break;
    case SealedExchangeState::kGenerated:
      base = "generated";
      break;
    case SealedExchangeState::kWaitingRemote:
      base = "waiting_remote";
      break;
    case SealedExchangeState::kReceived:
      base = "received";
      break;
    case SealedExchangeState::kApplied:
      base = "applied";
      break;
    case SealedExchangeState::kFailed:
      base = "failed";
      break;
  }

  if (detail.isEmpty()) {
    return base;
  }

  return QString("%1: %2").arg(base, detail);
}

class RuntimeProcessController {
public:
  RuntimeProcessController(QStringList gui_arguments, QString previous_context)
      : gui_arguments_(std::move(gui_arguments)), maintenance_context_(std::move(previous_context)) {
    process_.setProcessChannelMode(QProcess::SeparateChannels);
  }

  ~RuntimeProcessController() {
    stop();
  }

  bool is_running() const {
    return process_.state() != QProcess::NotRunning;
  }

  QProcess& process() {
    return process_;
  }
  void set_workspace_shutdown(std::function<void(std::function<void()>)> shutdown) {
    workspace_shutdown_ = std::move(shutdown);
  }

#ifdef _WIN32
  WorkspacePipeServer& workspace_pipe() { return workspace_pipe_; }
#endif

  bool start(const QString& program, const QStringList& args, const QString& role, const std::string& credential_frame, QString* error_detail) {
    if (is_running()) {
      if (error_detail != nullptr) {
        *error_detail = "Runtime process is already running.";
      }
      return false;
    }

    lifecycle_control_message_id_ = 0;
    stop_requested_ = false;
    const auto pipe_name = "RedClawAgent-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::string pipe_error;
    const bool pipe_started = agent_pipe_.start(pipe_name.toStdString(), true, 0, &pipe_error);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove("REDCLAW_AGENT_PIPE_NAME");
    environment.remove("REDCLAW_AGENT_PIPE_OWNER_PID");
    environment.remove("REDCLAW_WORKSPACE_PIPE_NAME");
    environment.remove("REDCLAW_WORKSPACE_PIPE_OWNER_PID");
    environment.remove("REDCLAW_RUNTIME_MAINTENANCE_CONTEXT");
#ifdef _WIN32
    QString maintenance_error;
    const bool maintenance_ready = role == "host" && maintenance_context_.prepare(&maintenance_error);
    if (maintenance_ready) environment.insert("REDCLAW_RUNTIME_MAINTENANCE_CONTEXT", maintenance_context_.path());
    const auto workspace_name = workspace_pipe_.listen();
    if (!workspace_name.isEmpty()) {
      environment.insert("REDCLAW_WORKSPACE_PIPE_NAME", workspace_name);
      environment.insert("REDCLAW_WORKSPACE_PIPE_OWNER_PID", QString::number(QCoreApplication::applicationPid()));
    }
#endif
    if (pipe_started) {
      environment.insert("REDCLAW_AGENT_PIPE_NAME", pipe_name);
      environment.insert("REDCLAW_AGENT_PIPE_OWNER_PID", QString::number(QCoreApplication::applicationPid()));
    }
    process_.setProcessEnvironment(environment);
    QStringList child_arguments = args;
    child_arguments.append("--gui-runtime-stdio");
    process_.start(program, child_arguments);
    if (!process_.waitForStarted(3000)) {
      if (error_detail != nullptr) {
        *error_detail = QString("Failed to start runtime process: %1")
                            .arg(process_.errorString());
      }
      return false;
    }

    if (process_.write(credential_frame.data(), static_cast<qint64>(credential_frame.size())) != static_cast<qint64>(credential_frame.size())
        || (process_.bytesToWrite() > 0 && !process_.waitForBytesWritten(3000))) {
      if (error_detail) *error_detail = "Could not deliver connection credentials to Runtime.";
      process_.kill(); process_.waitForFinished(3000); return false;
    }
    agent_pipe_.set_expected_peer_pid(static_cast<std::uint32_t>(process_.processId()));
#ifdef _WIN32
    if (maintenance_ready && !maintenance_context_.save(gui_arguments_, args, static_cast<quint64>(process_.processId()), &maintenance_error)) {
      write_gui_log_sink("Host maintenance unavailable: " + maintenance_error);
    } else if (role == "host" && !maintenance_ready) {
      write_gui_log_sink("Host maintenance unavailable: " + maintenance_error);
    }
    workspace_pipe_.set_expected_runtime_pid(static_cast<quint32>(process_.processId()));
#endif
    if (error_detail != nullptr) {
      error_detail->clear();
    }
    return true;
  }

  bool send_control_message(
      const redclaw::protocol::StreamControlMessageV1& message,
      QString* error_detail = nullptr) {
    return write_runtime_control_message(process_, message, error_detail);
  }

  bool send_agent_message(
      const redclaw::protocol::AgentMessageEnvelopeV1& message,
      QString* error_detail = nullptr) {
    std::string error;
    const bool sent = agent_pipe_.send(message, &error);
    if (error_detail) *error_detail = QString::fromStdString(error);
    return sent;
  }

  std::vector<redclaw::protocol::AgentMessageEnvelopeV1> take_agent_messages() {
    return agent_pipe_.take_received(1);
  }

  redclaw::agent::LocalAgentPipeStats agent_pipe_stats() const { return agent_pipe_.stats(); }

  void request_stop(bool end_desktop = true) {
    if (!is_running() || stop_requested_) {
      return;
    }
    stop_requested_ = true;
    request_remote_input_release();
    if (end_desktop && workspace_shutdown_) workspace_shutdown_([this] { terminate_after_workspace(); });
    else terminate_after_workspace();
  }

  void terminate_after_workspace() {
    const auto stopping_pid = process_.processId();
    QTimer::singleShot(100, &process_, [this, stopping_pid]() {
      if (is_running() && process_.processId() == stopping_pid) {
        process_.terminate();
      }
      QTimer::singleShot(2500, &process_, [this, stopping_pid]() {
        if (is_running() && process_.processId() == stopping_pid) {
          process_.kill();
        }
      });
    });
  }

  void stop() {
    if (!is_running()) {
      return;
    }

    request_remote_input_release();
    (void)process_.waitForBytesWritten(100);
    QThread::msleep(100);
    process_.terminate();
    if (!process_.waitForFinished(2500)) {
      process_.kill();
      process_.waitForFinished(1500);
    }
  }

private:
  void request_remote_input_release() {
    redclaw::protocol::StreamControlMessageV1 release;
    release.type = redclaw::protocol::StreamControlMessageTypeV1::kInputReleaseAll;
    release.session_epoch = "local";
    release.message_id = ++lifecycle_control_message_id_;
    release.sent_at_ms = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    (void)send_control_message(release, nullptr);
  }

  QProcess process_;
  redclaw::agent::LocalAgentPipe agent_pipe_;
  QStringList gui_arguments_;
  RuntimeMaintenanceContext maintenance_context_;
#ifdef _WIN32
  WorkspacePipeServer workspace_pipe_;
#endif
  std::uint64_t lifecycle_control_message_id_ = 0;
  bool stop_requested_ = false;
  std::function<void(std::function<void()>)> workspace_shutdown_;
};

QString service_lifecycle_error_to_text(redclaw::service::ServiceLifecycleError error) {
  using redclaw::service::ServiceLifecycleError;
  switch (error) {
    case ServiceLifecycleError::kNone:
      return "none";
    case ServiceLifecycleError::kInvalidConfig:
      return "invalid_config";
    case ServiceLifecycleError::kUnsupportedPlatform:
      return "unsupported_platform";
    case ServiceLifecycleError::kCommandFailed:
      return "command_failed";
  }

  return "unknown";
}

#if defined(_WIN32)
struct ScopedServiceHandle {
  SC_HANDLE handle = nullptr;

  ScopedServiceHandle() = default;
  explicit ScopedServiceHandle(SC_HANDLE value) : handle(value) {}

  ScopedServiceHandle(const ScopedServiceHandle&) = delete;
  ScopedServiceHandle& operator=(const ScopedServiceHandle&) = delete;

  ScopedServiceHandle(ScopedServiceHandle&& other) noexcept : handle(other.handle) {
    other.handle = nullptr;
  }

  ScopedServiceHandle& operator=(ScopedServiceHandle&& other) noexcept {
    if (this == &other) {
      return *this;
    }

    if (handle != nullptr) {
      CloseServiceHandle(handle);
    }

    handle = other.handle;
    other.handle = nullptr;
    return *this;
  }

  ~ScopedServiceHandle() {
    if (handle != nullptr) {
      CloseServiceHandle(handle);
    }
  }

  [[nodiscard]] bool valid() const {
    return handle != nullptr;
  }
};
#endif

struct ServiceReadinessState {
  QString binary_exists = "unknown";
  QString service_exists = "unknown";
  QString service_running = "unknown";
  QString summary = "Pending check";
};

ServiceReadinessState inspect_service_readiness(const QString& binary_path, const QString& service_name) {
  ServiceReadinessState state;

  const QFileInfo binary_info(binary_path.trimmed());
  state.binary_exists = binary_info.exists() ? "yes" : "no";

  if (service_name.trimmed().isEmpty()) {
    state.service_exists = "invalid service name";
    state.service_running = "unknown";
    state.summary = "Service name is required";
    return state;
  }

#if defined(_WIN32)
  const std::wstring service_name_w = service_name.trimmed().toStdWString();
  const ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!scm.valid()) {
    state.service_exists = "unavailable";
    state.service_running = "unknown";
    state.summary = "Cannot query SCM";
    return state;
  }

  const ScopedServiceHandle service(OpenServiceW(
      scm.handle,
      service_name_w.c_str(),
      SERVICE_QUERY_STATUS));
  if (!service.valid()) {
    state.service_exists = "no";
    state.service_running = "unknown";
    state.summary = "Service is not installed";
    return state;
  }

  state.service_exists = "yes";

  SERVICE_STATUS_PROCESS status = {};
  DWORD bytes_needed = 0;
  const BOOL query_result = QueryServiceStatusEx(
      service.handle,
      SC_STATUS_PROCESS_INFO,
      reinterpret_cast<LPBYTE>(&status),
      sizeof(status),
      &bytes_needed);
  if (query_result != 0) {
    state.service_running = status.dwCurrentState == SERVICE_RUNNING ? "yes" : "no";
    state.summary = status.dwCurrentState == SERVICE_RUNNING ? "Service is installed and running" : "Service is installed but not running";
    return state;
  }

  state.service_running = "unknown";
  state.summary = "Service is installed, status query failed";
  return state;
#else
  state.service_exists = "unsupported";
  state.service_running = "unsupported";
  state.summary = "Service status checks are Windows-only";
  return state;
#endif
}

}  // namespace

bool launch_gui_shell(
    int argc,
    char** argv,
    redclaw::diag::ProcessFileLogger* process_logger,
    std::string* error_detail) {
  MeasuredGuiApplication app(argc, argv);
  GuiLatencyScope initialization_timing(GuiStage::kGuiInitialize);
  GuiAutoStartOptions gui_auto_start;
  QString gui_auto_start_error;
  QStringList maintenance_gui_arguments = QCoreApplication::arguments();
  QStringList maintenance_runtime_arguments;
  QString maintenance_previous_context;
  const auto resume_index = maintenance_gui_arguments.indexOf("--gui-maintenance-resume");
  if (resume_index >= 0) {
    const auto snapshot = resume_index + 1 < maintenance_gui_arguments.size()
        ? RuntimeMaintenanceContext::read(maintenance_gui_arguments[resume_index + 1], &gui_auto_start_error) : std::nullopt;
    if (!snapshot) {
      if (error_detail) *error_detail = gui_auto_start_error.isEmpty() ? "maintenance_context_missing" : gui_auto_start_error.toStdString();
      return false;
    }
    maintenance_gui_arguments = snapshot->gui_arguments;
    maintenance_runtime_arguments = snapshot->runtime_arguments;
    maintenance_previous_context = QCoreApplication::arguments()[resume_index + 1];
  }
  auto gui_parse_arguments = maintenance_gui_arguments;
  if (!maintenance_runtime_arguments.isEmpty()) {
    gui_parse_arguments.append(maintenance_runtime_arguments);
    gui_parse_arguments << "--gui-auto-start" << "--gui-role" << "host";
  }
  if (!parse_gui_auto_start_options(gui_parse_arguments, &gui_auto_start, &gui_auto_start_error)) {
    if (error_detail != nullptr) {
      *error_detail = gui_auto_start_error.toStdString();
    }
    return false;
  }
  GuiDiagnosticWriter diagnostic_writer([process_logger, echo = gui_auto_start.auto_start](
      const GuiDiagnosticWriter::LogRecord& record, QString* error) {
    if (!record.append_path.isEmpty()) {
      QFile file(record.append_path);
      const bool ok = file.open(QIODevice::WriteOnly | QIODevice::Append)
          && file.write(record.text) == record.text.size() && file.flush();
      if (!ok && error) *error = file.errorString();
      return ok;
    }
    if (echo) std::cout << record.text.toStdString() << '\n';
    else if (process_logger) process_logger->write_line(record.source.toStdString(), record.text.toStdString(), record.flush);
    const bool ok = (!echo || std::cout.good()) && process_logger && process_logger->is_healthy();
    if (!ok && error) *error = "GUI diagnostic log write failed";
    return ok;
  });
  g_diagnostic_writer = &diagnostic_writer;
  QTimer latency_heartbeat(&app);
  QObject::connect(&latency_heartbeat, &QTimer::timeout, &app, [&app] { app.latency.heartbeat(); });
  latency_heartbeat.start(10);
  QTimer latency_window(&app);
  QObject::connect(&latency_window, &QTimer::timeout, &app, [&app] {
    const auto sample = app.latency.close_window();
    write_gui_log_sink("GUI scheduling " + QString::fromUtf8(QJsonDocument(sample).toJson(QJsonDocument::Compact)));
  });
  latency_window.start(1000);
  app.setStyleSheet(R"(
    QMainWindow, QWidget {
      background-color: #08111f;
      color: #e2e8f0;
      font: 10pt "Segoe UI";
    }
    QScrollArea {
      border: none;
      background: transparent;
    }
    QTabWidget::pane {
      border: 1px solid #1e293b;
      border-radius: 20px;
      background: #0f172a;
      top: -1px;
    }
    QTabBar::tab {
      background: #0b1220;
      color: #94a3b8;
      padding: 12px 22px;
      margin-right: 6px;
      border: 1px solid #1e293b;
      border-bottom: none;
      border-top-left-radius: 12px;
      border-top-right-radius: 12px;
    }
    QTabBar::tab:selected {
      background: #162033;
      color: #f8fafc;
    }
    QGroupBox {
      border: 1px solid #1f2a3d;
      border-radius: 16px;
      margin-top: 16px;
      padding: 14px;
      background-color: #0f172a;
      font: 600 10pt "Segoe UI";
    }
    QGroupBox::title {
      subcontrol-origin: margin;
      left: 16px;
      padding: 0 8px;
      color: #e2e8f0;
    }
    QPushButton {
      background-color: #132238;
      border: 1px solid #2a4a6b;
      border-radius: 12px;
      padding: 10px 18px;
      color: #e2e8f0;
      font: 600 10pt "Segoe UI";
    }
    QPushButton:hover {
      background-color: #1b3251;
    }
    QPushButton:pressed {
      background-color: #0f2138;
    }
    QPushButton:disabled {
      color: #64748b;
      border-color: #243244;
      background-color: #0f172a;
    }
    QToolButton {
      background: transparent;
      border: none;
      color: #7dd3fc;
      font-weight: 600;
      padding: 6px 2px;
      text-align: left;
    }
    QScrollBar:vertical {
      background-color: #08111f;
      width: 12px;
      margin: 2px 1px;
      border: none;
      border-radius: 6px;
    }
    QScrollBar::handle:vertical {
      background-color: #31506f;
      min-height: 32px;
      margin: 2px;
      border: none;
      border-radius: 4px;
    }
    QScrollBar::handle:vertical:hover {
      background-color: #3b82f6;
    }
    QScrollBar:horizontal {
      background-color: #08111f;
      height: 12px;
      margin: 1px 2px;
      border: none;
      border-radius: 6px;
    }
    QScrollBar::handle:horizontal {
      background-color: #31506f;
      min-width: 32px;
      margin: 2px;
      border: none;
      border-radius: 4px;
    }
    QScrollBar::handle:horizontal:hover {
      background-color: #3b82f6;
    }
    QScrollBar::add-line,
    QScrollBar::sub-line {
      width: 0;
      height: 0;
      background: transparent;
      border: none;
    }
    QScrollBar::add-page,
    QScrollBar::sub-page,
    QScrollBar::corner {
      background: transparent;
      border: none;
    }
    QLineEdit, QPlainTextEdit, QComboBox, QSpinBox, QListWidget {
      background-color: #0b1220;
      color: #e2e8f0;
      border: 1px solid #243244;
      border-radius: 12px;
      padding: 8px 10px;
      selection-background-color: #2563eb;
    }
    QFrame#localCodeCard {
      background-color: #08101d;
      border: 1px solid #2a3a51;
      border-radius: 12px;
    }
    QLabel#localCodeValue {
      color: #cbd5e1;
      font: 600 13pt "Consolas";
    }
    QLabel#fieldHelper {
      color: #94a3b8;
      font: 9pt "Segoe UI";
    }
    QLabel#fieldHelper[invalid="true"] {
      color: #fca5a5;
    }
    QLabel#connectionPhase {
      color: #dbeafe;
      background-color: rgba(37, 99, 235, 0.14);
      border: 1px solid #31598a;
      border-radius: 12px;
      padding: 9px 12px;
      font: 600 10pt "Segoe UI";
    }
    QLabel#progressCode {
      color: #93c5fd;
      font: 600 11pt "Consolas";
    }
    QLabel#connectionStageNumber {
      color: #bfdbfe;
      font: 700 11pt "Segoe UI";
    }
    QLabel#connectionStageTitle {
      color: #f8fafc;
      font: 600 10pt "Segoe UI";
    }
    QLabel#connectionStageState {
      color: #cbd5e1;
      font: 600 9pt "Segoe UI";
    }
    QLabel#connectionStageArrow {
      background-color: transparent;
      border: none;
    }
    QLabel#connectionFailure {
      color: #fee2e2;
      background-color: rgba(127, 29, 29, 0.42);
      border: 1px solid #ef4444;
      border-radius: 12px;
      padding: 9px 12px;
    }
    QLabel#heroTitle {
      font: 700 18pt "Segoe UI";
      color: #f8fafc;
    }
    QLabel#heroSubtitle {
      color: #cbd5e1;
      font: 10pt "Segoe UI";
    }
    QLabel#workflowHint {
      color: #93c5fd;
      font: 600 9pt "Segoe UI";
    }
    QLabel#pageIntro {
      color: #9fb3cc;
      font: 9pt "Segoe UI";
    }
    QWidget#heroPanel {
      background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                  stop:0 #10203a,
                                  stop:0.55 #0f172a,
                                  stop:1 #162c46);
      border: 1px solid #2a4263;
      border-radius: 18px;
    }
    QGroupBox#primaryCard {
      border-color: #27446b;
      background-color: #0d1728;
    }
    QGroupBox#secondaryCard {
      margin-top: 0;
      padding: 4px;
      border-color: #243b5a;
      background-color: #0b1423;
    }
    QGroupBox#primaryCard[persistent="true"] {
      padding: 0;
    }
    QFrame#accentStrip {
      background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                                  stop:0 #22d3ee,
                                  stop:0.5 #3b82f6,
                                  stop:1 #14b8a6);
      border-radius: 999px;
      min-height: 6px;
      max-height: 6px;
    }
    QLabel#heroEyebrow {
      color: #7dd3fc;
      font: 600 9pt "Segoe UI";
      letter-spacing: 0.08em;
    }
    QLabel#featureChip {
      background-color: rgba(37, 99, 235, 0.18);
      color: #dbeafe;
      border: 1px solid #31598a;
      border-radius: 999px;
      padding: 4px 10px;
    }
    QLabel#statusBanner {
      background-color: rgba(37, 99, 235, 0.14);
      color: #dbeafe;
      border: 1px solid #31598a;
      border-radius: 14px;
      padding: 10px 12px;
    }
    QPlainTextEdit#machineCodeBox {
      font: 600 13pt "Consolas";
      border-color: #31598a;
      background-color: #08101d;
      padding: 12px 14px;
    }
    QPushButton#primaryAction {
      background-color: #0f766e;
      border-color: #14b8a6;
      color: #f8fafc;
      font: 600 10pt "Segoe UI";
      padding: 12px 18px;
    }
    QPushButton#primaryAction:hover {
      background-color: #109189;
    }
    QPushButton#connectAction {
      background-color: #2563eb;
      border-color: #60a5fa;
      color: #f8fafc;
      font: 600 10pt "Segoe UI";
      padding: 12px 18px;
    }
    QPushButton#connectAction:hover {
      background-color: #2f74f0;
    }
    QPushButton#primaryAction:disabled,
    QPushButton#connectAction:disabled {
      background-color: #172033;
      color: #64748b;
      border-color: #243244;
    }
    QPushButton#exitAction {
      background-color: transparent;
      border-color: #64748b;
      color: #e2e8f0;
      min-width: 110px;
    }
    QPushButton#secondaryAction {
      background-color: #132238;
      border-color: #334155;
      padding: 7px 12px;
    }
    QFrame#statusCard {
      background-color: #0a1424;
      border: 1px solid #243b59;
      border-radius: 18px;
    }
    QLabel#statusCardTitle {
      color: #94a3b8;
      font: 600 9pt "Segoe UI";
    }
    QLabel#statusCardValue {
      color: #f8fafc;
      font: 600 14pt "Segoe UI";
    }
    QWidget#playbackWindow {
      background-color: #030712;
    }
    QLabel#playbackWindowStatus {
      color: #94a3b8;
      font: 10pt "Segoe UI";
    }
    QWidget#playbackWindowCanvas {
      background-color: #020617;
      color: #cbd5e1;
      border: 1px solid #334155;
      border-radius: 18px;
    }
    QWidget#agentConversationPanel {
      background-color: #0b1423;
      border: 1px solid #263a57;
      border-radius: 10px;
    }
    QLabel#agentPanelTitle {
      color: #f8fafc;
      font: 700 13pt "Segoe UI";
    }
    QLabel#agentConnectionState {
      color: #fca5a5;
      background-color: rgba(127, 29, 29, 0.32);
      border: 1px solid #7f1d1d;
      border-radius: 9px;
      padding: 3px 7px;
      font: 600 8pt "Segoe UI";
    }
    QLabel#agentConnectionState[connected="true"] {
      color: #a7f3d0;
      background-color: rgba(6, 95, 70, 0.32);
      border-color: #0f766e;
    }
    QToolButton#agentContextToggle {
      color: #9fb3cc;
      border: 1px solid #243b59;
      border-radius: 10px;
      background-color: #0a1424;
      padding: 7px 9px;
    }
    QFrame#agentContextBox {
      background-color: #08111f;
      border: 1px solid #243b59;
      border-radius: 10px;
    }
    QLabel#agentPanelStatus, QLabel#agentComposerHint,
    QLabel#agentConversationEmpty, QLabel#agentSystemMessage {
      color: #94a3b8;
      font: 9pt "Segoe UI";
    }
    QLabel#agentPanelStatus[error="true"], QLabel#agentSystemError {
      color: #fecaca;
    }
    QScrollArea#agentConversationScroll, QWidget#agentConversationContents {
      background-color: transparent;
      border: none;
    }
    QFrame#agentUserBubble {
      background-color: transparent;
      border: 1px solid #3b82f6;
      border-radius: 12px;
    }
    QFrame#agentUserBubble QLabel {
      background-color: transparent;
      border: none;
    }
    QTextBrowser#agentReplyMarkdown, QPlainTextEdit#agentReplyPlain {
      background-color: transparent;
      color: #e2e8f0;
      border: none;
      padding: 4px 2px;
    }
    QFrame#agentActivityGroup, QFrame#agentActivityFailed {
      background-color: #0a1424;
      border: 1px solid #243b59;
      border-radius: 9px;
    }
    QFrame#agentActivityFailed {
      border-color: #b45309;
      background-color: rgba(120, 53, 15, 0.22);
    }
    QLabel#agentActivityDetails {
      color: #94a3b8;
      font: 9pt "Consolas";
    }
    QFrame#agentApprovalBanner {
      background-color: rgba(120, 53, 15, 0.30);
      border: 1px solid #f59e0b;
      border-radius: 11px;
    }
    QPlainTextEdit#agentComposer {
      background-color: #08111f;
      border-color: #31598a;
      border-radius: 11px;
    }
    QPushButton#agentSendAction, QPushButton#agentApproveAction {
      background-color: #2563eb;
      border-color: #60a5fa;
      padding: 7px 14px;
    }
    QPushButton#agentPanelSecondaryAction {
      background-color: #132238;
      border-color: #334155;
      padding: 7px 11px;
    }
  )");

  RuntimeProcessController controller(maintenance_gui_arguments, maintenance_previous_context);

  QMainWindow window;
  window.setWindowTitle("RedClaw Desktop");
  window.resize(kDefaultWindowWidth, kDefaultWindowHeight);
  window.setMinimumSize(kMinimumWindowWidth, kMinimumWindowHeight);

  const QString process_log_path = process_logger == nullptr
      ? QString()
      : QString::fromStdString(process_logger->log_path().string());
  QString effective_log_dir = gui_auto_start.log_dir;
  if (effective_log_dir.isEmpty() && !process_log_path.isEmpty()) {
    effective_log_dir = QFileInfo(process_log_path).absolutePath();
  }
  if (effective_log_dir.isEmpty()) {
    effective_log_dir = QDir(QDir::tempPath()).filePath("RedClawDesktop/logs");
  }
  if (!QDir().mkpath(effective_log_dir)) {
    if (error_detail != nullptr) {
      *error_detail = QString("failed to create GUI log directory: %1").arg(effective_log_dir).toStdString();
    }
    g_diagnostic_writer = nullptr;
    return false;
  }

  const QString status_path = QDir(effective_log_dir).filePath("status.json");
  const QString evidence_manifest_path = QDir(effective_log_dir).filePath("evidence-manifest.json");
  const QString control_events_path = QDir(effective_log_dir).filePath("control-events.log");
  redclaw::diag::DebugRuntimeStatus debug_status;
  debug_status.run_id = gui_auto_start.run_id.toStdString();
  debug_status.role = gui_auto_start.role.toStdString();
  debug_status.network_bind_address = gui_auto_start.network_bind_address.isEmpty()
      ? "auto"
      : gui_auto_start.network_bind_address.toStdString();
  debug_status.app_pid = static_cast<std::uint64_t>(QCoreApplication::applicationPid());
  debug_status.started_at_unix_ms = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
  debug_status.updated_at_unix_ms = debug_status.started_at_unix_ms;
  debug_status.process_log_path = process_log_path.toStdString();
  debug_status.status_path = status_path.toStdString();
  debug_status.evidence_manifest_path = evidence_manifest_path.toStdString();
  qint64 last_status_write_ms = 0;
  std::uint64_t last_status_version = 0;

  auto persist_debug_status = [&](bool force) {
    GuiLatencyScope timing(GuiStage::kStatusWrite);
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    debug_status.updated_at_unix_ms = static_cast<std::uint64_t>(now_ms);
    if (!force && last_status_write_ms > 0 && now_ms - last_status_write_ms < 1000) {
      return last_status_version;
    }
    last_status_version = diagnostic_writer.submit_status(status_path, debug_runtime_status_work(debug_status));
    last_status_write_ms = now_ms;
    return last_status_version;
  };

  auto append_control_event = [&](const QString& request_id,
                                  const QString& action,
                                  bool ok,
                                  const QString& error_code) {
    QJsonObject event;
    event.insert("timestamp", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    event.insert("request_id", request_id);
    event.insert("action", action);
    event.insert("ok", ok);
    event.insert("error_code", error_code);
    diagnostic_writer.post_log({"control-event", QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n', true, control_events_path});
  };

  persist_debug_status(true);
  auto* debug_status_timer = new QTimer(&window);
  debug_status_timer->setInterval(1000);
  QObject::connect(debug_status_timer, &QTimer::timeout, [&]() {
    debug_status.runtime_running = controller.is_running();
    debug_status.runtime_pid = controller.is_running()
        ? static_cast<std::uint64_t>(controller.process().processId())
        : 0;
    persist_debug_status(false);
  });
  debug_status_timer->start();

  auto* shell = new QWidget(&window);
  auto* shell_layout = new QVBoxLayout(shell);
  shell_layout->setContentsMargins(8, 8, 8, 8);
  shell_layout->setSpacing(8);

  auto* tabs = new ConnectionViewStack(shell);

  auto* session_page = new QWidget();
  session_page->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto* session_layout = new QVBoxLayout(session_page);
  session_layout->setContentsMargins(6, 6, 6, 6);
  session_layout->setSpacing(0);

  auto* connection_entry_page = new ConnectionEntryPage(session_page);
  // The existing Debug-only fixture must not overwrite live connection,
  // permission or layout preferences while isolated GUIs share this user.
  auto* ui_settings = gui_auto_start.agent_qa_fixture_provider
      ? new QSettings(QDir(effective_log_dir).filePath("qa-ui-settings.ini"), QSettings::IniFormat, &window)
      : new QSettings("RedClaw", "RedClawDesktop", &window);
  connection_entry_page->password_panel()->set_settings(ui_settings);
  QString connection_credential_error;
  QString connection_auth_failure;
  if (!gui_auto_start.connection_credential_file.isEmpty()) {
    redclaw::security::ConnectionCredential credential;
    if (!redclaw::security::load_connection_credential_file(
          std::filesystem::path(gui_auto_start.connection_credential_file.toStdWString()), &credential)) {
      connection_credential_error = "Encrypted connection credential is unavailable. Recreate it for this Windows user.";
    } else connection_entry_page->password_panel()->set_credential_override(std::move(credential));
  }
  const int saved_ice_udp_port = ui_settings->value("network/ice_udp_port", 55000).toInt();
  const int initial_ice_udp_port = gui_auto_start.ice_udp_port_explicit
      ? static_cast<int>(gui_auto_start.ice_udp_port)
      : (saved_ice_udp_port >= 1 && saved_ice_udp_port <= 65535
          ? saved_ice_udp_port
          : 55000);
  debug_status.ice_udp_port = static_cast<std::uint16_t>(initial_ice_udp_port);
  auto* agent_settings_dialog = new AgentSettingsDialog(ui_settings, &window);
  connection_entry_page->allow_remote_control_checkbox()->setChecked(
      ui_settings->value("host/allow_remote_control", false).toBool());
  connection_entry_page->allow_remote_agent_checkbox()->setChecked(
      ui_settings->value("host/allow_remote_agent", false).toBool());
  QObject::connect(
      connection_entry_page->allow_remote_control_checkbox(),
      &QCheckBox::toggled,
      &window,
      [ui_settings](bool checked) {
        ui_settings->setValue("host/allow_remote_control", checked);
      });
  QObject::connect(
      connection_entry_page->allow_remote_agent_checkbox(),
      &QCheckBox::toggled,
      &window,
      [ui_settings](bool checked) {
        ui_settings->setValue("host/allow_remote_agent", checked);
      });
  bool host_wait_remote_input_authorized = false;
  connection_entry_page->allow_controller_agent_checkbox()->setChecked(
      ui_settings->value("controller/allow_remote_agent", false).toBool());
  QObject::connect(connection_entry_page->allow_controller_agent_checkbox(),
      &QCheckBox::toggled, &window, [ui_settings](bool checked) {
        ui_settings->setValue("controller/allow_remote_agent", checked);
      });
  bool host_wait_remote_agent_authorized = false;
  session_layout->addWidget(connection_entry_page);
  auto* session_hero = connection_entry_page->hero_panel();

  auto* session_form_box = new QGroupBox("Advanced Session Settings", session_page);
  auto* session_form = new QFormLayout(session_form_box);

  auto* role_combo = new QComboBox(session_form_box);
  role_combo->addItem("host");
  role_combo->addItem("controller");

  auto* transport_combo = new QComboBox(session_form_box);
  transport_combo->addItem("file");
  transport_combo->addItem("event-log");
  transport_combo->addItem("tcp");
  transport_combo->addItem("sealed-file");
  transport_combo->addItem("rendezvous");
  transport_combo->addItem("dht");

  auto* target_host = new QLineEdit(session_form_box);
  target_host->setPlaceholderText("Controller mode with tcp requires target host");

  auto* target_port = new QSpinBox(session_form_box);
  target_port->setRange(0, 65535);
  target_port->setValue(45909);

  auto* rendezvous_url = new QLineEdit(session_form_box);
  rendezvous_url->setPlaceholderText("Required for rendezvous transport, e.g. https://rendezvous.example.com/api");

  auto* session_code = new QLineEdit(session_form_box);
  session_code->setPlaceholderText("Peer machine code (8 chars, A-Z0-9)");

  auto* dht_bootstrap = new QPlainTextEdit(session_form_box);
  dht_bootstrap->setPlaceholderText("One DHT bootstrap node per line, e.g. router.bittorrent.com:6881");
  dht_bootstrap->setPlainText(
      "router.bittorrent.com:6881\n"
      "dht.transmissionbt.com:6881\n"
      "dht.libtorrent.org:25401\n"
      "router.utorrent.com:6881");
  dht_bootstrap->setMaximumHeight(70);

  auto* enable_port_mapping = new QCheckBox("Try UPnP port mapping", session_form_box);
  enable_port_mapping->setChecked(false);
  enable_port_mapping->setToolTip(
      "Maps the configured ICE UDP port. DHT ports are not mapped.");

  auto* enable_ipv6_candidates = new QCheckBox("Enable IPv6 candidate diagnostics", session_form_box);
  enable_ipv6_candidates->setChecked(true);

  auto* helper_role_combo = new QComboBox(session_form_box);
  helper_role_combo->addItem("off");
  helper_role_combo->addItem("client");
  helper_role_combo->addItem("lan-helper");
  helper_role_combo->addItem("relay");
  helper_role_combo->setCurrentText("client");

  auto* signal_timeout = new QSpinBox(session_form_box);
  signal_timeout->setRange(0, 3600);
  signal_timeout->setValue(0);
  signal_timeout->setSpecialValueText("No limit");

  auto* run_seconds = new QSpinBox(session_form_box);
  run_seconds->setRange(0, 24 * 3600);
  run_seconds->setValue(0);
  run_seconds->setToolTip("0 means long-running mode");

  auto* signal_dir = new QLineEdit(session_form_box);
  signal_dir->setText("runtime-signaling");

  auto* signal_passphrase = new QLineEdit(session_form_box);
  signal_passphrase->setEchoMode(QLineEdit::Password);
  signal_passphrase->setPlaceholderText("Required for sealed-file (min 16 chars)");

  auto* runtime_config_path = new QLineEdit(session_form_box);
  runtime_config_path->setPlaceholderText("Optional runtime profile path (key=value file)");

  auto* runtime_config_browse = new QPushButton("Browse", session_form_box);

  auto* runtime_config_row = new QHBoxLayout();
  runtime_config_row->addWidget(runtime_config_path, 1);
  runtime_config_row->addWidget(runtime_config_browse);

  QObject::connect(
      connection_entry_page->agent_settings_button(),
      &QPushButton::clicked,
      &window,
      [&]() {
        agent_settings_dialog->set_runtime_active(controller.is_running());
        agent_settings_dialog->refresh_provider_status();
        agent_settings_dialog->exec();
      });

  auto* runtime_form_override = new QCheckBox("Use form values to override runtime profile", session_form_box);
  runtime_form_override->setChecked(true);

  auto* additional_ice_servers = new QPlainTextEdit(session_form_box);
  additional_ice_servers->setPlaceholderText(
      "Optional additional ICE/TURN URI per line. Use a local file for secret-bearing TURN credentials.");
  additional_ice_servers->setMaximumHeight(96);

  auto* enable_ice_tcp = new QCheckBox("Enable ICE TCP candidates", session_form_box);
  enable_ice_tcp->setChecked(false);

  auto* stream_smoke = new QCheckBox("Enable desktop stream workflow", session_form_box);
  stream_smoke->setChecked(true);

  auto* stream_require_capture = new QCheckBox("Require real host capture", session_form_box);
  stream_require_capture->setChecked(true);

  auto* allow_remote_diagnostics = new QCheckBox(
      "Allow the connected peer to request redacted logs", session_form_box);
#if defined(NDEBUG)
  allow_remote_diagnostics->setChecked(false);
#else
  allow_remote_diagnostics->setChecked(true);
#endif

  auto* stream_preview_width = new QSpinBox(session_form_box);
  stream_preview_width->setRange(16, 640);
  stream_preview_width->setValue(160);

  auto* stream_video_max_width = new QSpinBox(session_form_box);
  stream_video_max_width->setRange(0, 7680);
  stream_video_max_width->setValue(0);
  stream_video_max_width->setToolTip("0 keeps native capture width; lower values are a manual weak-network diagnostic cap.");

  session_form->addRow("Role", role_combo);
  session_form->addRow("Transport", transport_combo);
  session_form->addRow("Target host", target_host);
  session_form->addRow("Target port", target_port);
  session_form->addRow("Rendezvous URL", rendezvous_url);
  session_form->addRow("Session code", session_code);
  session_form->addRow("DHT bootstrap", dht_bootstrap);
  session_form->addRow("ICE UDP mapping", enable_port_mapping);
  session_form->addRow("IPv6 candidates", enable_ipv6_candidates);
  session_form->addRow("Helper role", helper_role_combo);
  session_form->addRow("Signal timeout (s)", signal_timeout);
  session_form->addRow("Run seconds", run_seconds);
  session_form->addRow("Signal directory", signal_dir);
  session_form->addRow("Signal passphrase", signal_passphrase);
  session_form->addRow("Runtime profile", runtime_config_row);
  session_form->addRow("Profile behavior", runtime_form_override);
  session_form->addRow("Additional ICE/TURN", additional_ice_servers);
  session_form->addRow("ICE TCP", enable_ice_tcp);
  session_form->addRow("Desktop stream", stream_smoke);
  session_form->addRow("Capture gate", stream_require_capture);
  session_form->addRow("Remote diagnostics", allow_remote_diagnostics);
  session_form->addRow("Preview width", stream_preview_width);
  session_form->addRow("Video max width", stream_video_max_width);

  auto* action_row = new QHBoxLayout();
  auto* start_button = new QPushButton("Start Runtime", session_form_box);
  auto* stop_button = new QPushButton("Stop Runtime", session_form_box);
  stop_button->setEnabled(false);
  action_row->addWidget(start_button);
  action_row->addWidget(stop_button);
  session_form->addRow("Actions", action_row);

  auto* status_label = new QLabel("Not running", session_form_box);
  session_form->addRow("Runtime status", status_label);

  auto write_agent_project_manifest = [&](QString* manifest_path, QString* error_detail) {
    QStringList roots;
    for (const auto& root : agent_settings_dialog->project_roots()) {
      const QFileInfo project(QDir::cleanPath(root.trimmed()));
      if (!project.exists() || !project.isDir()) {
        if (error_detail != nullptr) {
          *error_detail = "Every registered Agent project must be an existing local directory.";
        }
        return false;
      }
      const QString canonical = project.canonicalFilePath();
      if (!canonical.isEmpty() && !roots.contains(canonical, Qt::CaseInsensitive)) {
        roots.push_back(canonical);
      }
    }
    if (roots.isEmpty()) {
      if (error_detail != nullptr) {
        *error_detail = "Agent access requires at least one registered local project.";
      }
      return false;
    }
    QDir directory(QDir::current().absoluteFilePath(signal_dir->text().trimmed()));
    if (!directory.mkpath(".")) {
      if (error_detail != nullptr) {
        *error_detail = "Could not create the local Agent project manifest directory.";
      }
      return false;
    }
    const QString output_path = directory.absoluteFilePath("agent-projects.conf");
    QSaveFile output(output_path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      if (error_detail != nullptr) {
        *error_detail = "Could not open the local Agent project manifest.";
      }
      return false;
    }
    for (const QString& root : roots) {
      const QString path_hash = QString::fromLatin1(
          QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex());
      const QString settings_key = QString("host/agent_project_ids/%1").arg(path_hash);
      QString project_id = ui_settings->value(settings_key).toString();
      if (project_id.isEmpty()) {
        project_id = "p-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        ui_settings->setValue(settings_key, project_id);
      }
      const QString display_name = QFileInfo(root).fileName().isEmpty()
          ? "Registered project" : QFileInfo(root).fileName();
      output.write(QString("%1|%2|%3\n").arg(project_id, display_name, root).toUtf8());
    }
    if (!output.commit()) {
      if (error_detail != nullptr) {
        *error_detail = "Could not commit the local Agent project manifest.";
      }
      return false;
    }
    QFile::setPermissions(output_path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (manifest_path != nullptr) {
      *manifest_path = output_path;
    }
    return true;
  };

  auto* link_workflow_box = connection_entry_page->primary_card();
  auto* peer_machine_code = connection_entry_page->peer_code_input();
  auto* generate_local_code_button = connection_entry_page->wait_button();
  auto* connect_peer_button = connection_entry_page->connect_button();

  auto set_link_status = [&](const QString& text, const QString& tone) {
    connection_entry_page->set_status(text, tone);
  };
  set_link_status("Preparing your local code...", "info");

  generate_local_code_button->setToolTip("Start waiting on this device using the code shown above.");
  connect_peer_button->setToolTip("Paste the other device's code, then connect.");

  auto* stun_preset_combo = new PageScrollComboBox(connection_entry_page);
  stun_preset_combo->setObjectName("stunPresetCombo");
  for (const auto& preset : stun_server_presets()) {
    stun_preset_combo->addItem(preset.label, preset.id);
    stun_preset_combo->setItemData(
        stun_preset_combo->count() - 1,
        preset.detail,
        Qt::ToolTipRole);
  }
  stun_preset_combo->setCurrentIndex(
      stun_preset_combo->findData(default_stun_server_preset_id()));
  stun_preset_combo->setToolTip(
      stun_preset_combo->currentData(Qt::ToolTipRole).toString());
  QObject::connect(
      stun_preset_combo,
      &QComboBox::currentIndexChanged,
      connection_entry_page,
      [stun_preset_combo](int) {
        stun_preset_combo->setToolTip(
            stun_preset_combo->currentData(Qt::ToolTipRole).toString());
      });

  auto* link_network_exit = new PageScrollComboBox(connection_entry_page);
  populate_network_exit_combo(link_network_exit, gui_auto_start.network_bind_address);
  link_network_exit->setToolTip(
      "Auto follows the Windows route table. Manual selection binds DHT, ICE/STUN/TURN, and UPnP to one local IPv4 address.");

  auto* link_ice_udp_port = new QSpinBox(connection_entry_page);
  link_ice_udp_port->setRange(1, 65535);
  link_ice_udp_port->setValue(initial_ice_udp_port);
  link_ice_udp_port->setToolTip(
      "Fixed local UDP port used by ICE. Configure the same UDP port in a manual router mapping.");

  auto* link_enable_port_mapping = new QCheckBox("Try UPnP port mapping", connection_entry_page);
  link_enable_port_mapping->setChecked(true);
  link_enable_port_mapping->setToolTip(
      "Asks the local router to map the configured ICE UDP port. DHT ports are not mapped.");

  auto sync_link_network_settings_to_session_form = [&]() {
    enable_port_mapping->setChecked(link_enable_port_mapping->isChecked());
    enable_ice_tcp->setChecked(true);
  };

  connection_entry_page->network_settings_layout()->addRow("STUN service", stun_preset_combo);
  connection_entry_page->network_settings_layout()->addRow("Network exit", link_network_exit);
  connection_entry_page->network_settings_layout()->addRow("ICE UDP port", link_ice_udp_port);
  connection_entry_page->network_settings_layout()->addRow(QString(), link_enable_port_mapping);

  session_form_box->setVisible(false);

  auto* advanced_session_toggle = new QToolButton(session_page);
  advanced_session_toggle->setCheckable(true);
  advanced_session_toggle->setChecked(false);
  advanced_session_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  advanced_session_toggle->setArrowType(Qt::RightArrow);
  advanced_session_toggle->setText("Show advanced session settings");
  QObject::connect(advanced_session_toggle, &QToolButton::toggled, session_page, [advanced_session_toggle, session_form_box](bool expanded) {
    session_form_box->setVisible(expanded);
    advanced_session_toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    advanced_session_toggle->setText(expanded ? "Hide advanced session settings" : "Show advanced session settings");
  });
  advanced_session_toggle->setVisible(false);

  auto* sealed_exchange_box = new QGroupBox("Offline / Sealed Exchange", session_page);
  auto* sealed_exchange_form = new QFormLayout(sealed_exchange_box);

  auto* sealed_exchange_status = new QLabel("Diagnostic path idle", sealed_exchange_box);
  auto* local_sealed_blob = new QPlainTextEdit(sealed_exchange_box);
  local_sealed_blob->setReadOnly(true);
  local_sealed_blob->setPlaceholderText("Local sealed blob will appear here (host-offer.sealed.txt or controller-answer.sealed.txt)");
  local_sealed_blob->setMaximumHeight(110);

  auto* refresh_local_blob_button = new QPushButton("Refresh Local Sealed Blob", sealed_exchange_box);

  auto* remote_sealed_blob = new QPlainTextEdit(sealed_exchange_box);
  remote_sealed_blob->setPlaceholderText("Paste remote sealed blob text, then click Apply");
  remote_sealed_blob->setMaximumHeight(110);

  auto* apply_remote_blob_button = new QPushButton("Apply Remote Sealed Blob", sealed_exchange_box);

  sealed_exchange_form->addRow("Exchange status", sealed_exchange_status);
  sealed_exchange_form->addRow("Local sealed blob", local_sealed_blob);
  sealed_exchange_form->addRow("Actions", refresh_local_blob_button);
  sealed_exchange_form->addRow("Remote sealed blob", remote_sealed_blob);
  sealed_exchange_form->addRow("", apply_remote_blob_button);

  sealed_exchange_box->setVisible(false);

  auto* sealed_exchange_toggle = new QToolButton(session_page);
  sealed_exchange_toggle->setCheckable(true);
  sealed_exchange_toggle->setChecked(false);
  sealed_exchange_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  sealed_exchange_toggle->setArrowType(Qt::RightArrow);
  sealed_exchange_toggle->setText("Show offline sealed-exchange tools");
  QObject::connect(sealed_exchange_toggle, &QToolButton::toggled, session_page, [sealed_exchange_box, sealed_exchange_toggle](bool expanded) {
    sealed_exchange_box->setVisible(expanded);
    sealed_exchange_toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    sealed_exchange_toggle->setText(expanded ? "Hide offline sealed-exchange tools" : "Show offline sealed-exchange tools");
  });
  sealed_exchange_toggle->setVisible(false);

  auto* session_log_box = new QGroupBox("Runtime Log", shell);
  session_log_box->setObjectName("primaryCard");
  session_log_box->setProperty("persistent", true);
  session_log_box->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
  session_log_box->setMinimumHeight(160);
  auto* session_log_layout = new QVBoxLayout(session_log_box);
  session_log_layout->setContentsMargins(12, 6, 12, 10);
  session_log_layout->setSpacing(6);
  auto* session_log_toolbar = new QHBoxLayout();
  auto* session_log_hint = new QLabel("Pairing, ICE, DHT, and stream events appear here during a session.", session_log_box);
  session_log_hint->setWordWrap(true);
  session_log_hint->setObjectName("pageIntro");
  session_log_hint->hide();
  auto* copy_session_log_button = new QPushButton("Copy Log", session_log_box);
  copy_session_log_button->setObjectName("secondaryAction");
  copy_session_log_button->setToolTip("Copy the full runtime log to the clipboard");
  auto* clear_session_log_button = new QPushButton("Clear", session_log_box);
  clear_session_log_button->setObjectName("secondaryAction");
  clear_session_log_button->setToolTip("Clear the on-screen log view");
  auto* session_log_copy_feedback = new QLabel(session_log_box);
  session_log_copy_feedback->setObjectName("statusCardTitle");
  session_log_toolbar->addWidget(copy_session_log_button);
  session_log_toolbar->addWidget(clear_session_log_button);
  session_log_toolbar->addStretch();
  session_log_toolbar->addWidget(session_log_copy_feedback);
  auto* session_log = new RuntimeLogView(session_log_box);
  session_log->setObjectName("runtimeSessionLog");
  session_log->setPlaceholderText("Runtime and pairing events appear here...");
  session_log->setMinimumHeight(84);
  session_log_layout->addWidget(session_log_hint);
  session_log_layout->addLayout(session_log_toolbar);
  session_log_layout->addWidget(session_log, 1);

  auto* session_scroll = new QScrollArea(tabs);
  session_scroll->setWidgetResizable(true);
  session_scroll->setFrameShape(QFrame::NoFrame);
  session_scroll->setWidget(session_page);
  session_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  session_scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  session_scroll->setMinimumSize(0, 0);

  tabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  tabs->setMinimumHeight(0);

  tabs->addWidget(session_scroll);
  auto* connecting_page = new ConnectionProgressPage(ConnectionFlowRole::kController, tabs);
  auto* waiting_page = new ConnectionProgressPage(ConnectionFlowRole::kHost, tabs);
  tabs->addWidget(connecting_page);
  tabs->addWidget(waiting_page);
  shell_layout->addWidget(tabs, 0);
  shell_layout->addWidget(session_log_box, 1);
  auto sync_connection_view_height = [
      shell,
      shell_layout,
      tabs,
      session_page,
      session_scroll,
      connection_entry_page]() {
    if (tabs->currentWidget() == session_scroll) {
      connection_entry_page->layout()->activate();
      session_page->layout()->activate();
      tabs->setMaximumHeight(session_page->sizeHint().height());
    } else {
      tabs->setMaximumHeight(QWIDGETSIZE_MAX);
    }
    tabs->updateGeometry();
    shell_layout->invalidate();
    shell->updateGeometry();
  };
  QObject::connect(
      connection_entry_page->network_settings_toggle(),
      &QToolButton::toggled,
      session_log_box,
      [session_scroll, session_log_box, sync_connection_view_height](bool expanded) {
        session_scroll->setVerticalScrollBarPolicy(
            expanded ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
        QTimer::singleShot(0, session_log_box, [session_log_box, sync_connection_view_height]() {
          sync_connection_view_height();
          session_log_box->show();
          session_log_box->raise();
          session_log_box->update();
        });
      });
  QObject::connect(tabs, &QStackedWidget::currentChanged, tabs, [sync_connection_view_height](int) {
    sync_connection_view_height();
  });
  QTimer::singleShot(0, tabs, sync_connection_view_height);

  ConnectionFlowModel connection_flow_model;
  auto refresh_connection_flow_page = [&]() {
    const ConnectionFlowSnapshot& snapshot = connection_flow_model.snapshot();
    connecting_page->set_snapshot(snapshot);
    waiting_page->set_snapshot(snapshot);
    if (snapshot.view == ConnectionFlowView::kConnecting) {
      tabs->setCurrentWidget(connecting_page);
    } else if (snapshot.view == ConnectionFlowView::kWaiting) {
      tabs->setCurrentWidget(waiting_page);
    } else {
      tabs->setCurrentWidget(session_scroll);
    }
  };
  auto apply_connection_flow_event = [&](ConnectionFlowEvent event, const QString& detail = QString()) {
    connection_flow_model.apply(event, detail);
    refresh_connection_flow_page();
  };

  auto* runtime_page = new QWidget(&window);
  runtime_page->setVisible(false);

  auto* runtime_overview = new QGroupBox("Session Status", session_page);
  runtime_overview->setObjectName("primaryCard");
  auto* runtime_cards = new QHBoxLayout(runtime_overview);
  runtime_cards->setSpacing(12);
  runtime_cards->setContentsMargins(6, 10, 6, 6);

  auto create_status_card = [&](const QString& title, const QString& initial_value) {
    auto* card = new QFrame(runtime_overview);
    card->setObjectName("statusCard");
    card->setMinimumWidth(170);
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(16, 14, 16, 14);
    card_layout->setSpacing(8);

    auto* title_label = new QLabel(title, card);
    title_label->setObjectName("statusCardTitle");
    auto* value_label = new QLabel(initial_value, card);
    value_label->setObjectName("statusCardValue");
    value_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    card_layout->addWidget(title_label);
    card_layout->addWidget(value_label);
    card_layout->addStretch();
    runtime_cards->addWidget(card, 1);
    return value_label;
  };

  auto* connection_state_value = create_status_card("Pairing", "Ready");
  auto* remote_desc_value = create_status_card("Handshake", "Waiting");
  auto* connected_value = create_status_card("Live Link", "Pending");
  auto* process_mode_value = create_status_card("App State", "Idle");

  session_layout->addWidget(runtime_overview);
  runtime_overview->setVisible(false);

  auto* playback_canvas = new QLabel(runtime_page);
  playback_canvas->setAlignment(Qt::AlignCenter);
  playback_canvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  playback_canvas->setText("Your remote desktop will appear here");
  playback_canvas->hide();
  auto* playback_status = new QLabel("The live desktop will appear here as soon as pairing completes.", runtime_page);
  playback_status->setWordWrap(true);
  playback_status->hide();

  // A QWidget parent plus Qt::Window is a Windows owned window: it stays above
  // its owner and is hidden when that owner is minimized. The playback window
  // has to be an independent top-level window so the two can switch z-order.
  auto* playback_window = new QWidget(nullptr, Qt::Window);
  struct PlaybackWindowOwner final : QObject {
    QWidget* playback = nullptr;
    PlaybackWindowOwner(QWidget* owner, QWidget* playback_window)
        : QObject(owner), playback(playback_window) {}
    ~PlaybackWindowOwner() override { delete playback; }
  };
  new PlaybackWindowOwner(&window, playback_window);
  playback_window->setObjectName("playbackWindow");
  playback_window->setWindowTitle("RedClaw");
  playback_window->setMinimumSize(360, 240);
  new PlaybackWindowLifecycle(playback_window, &window);
  auto* playback_window_layout = new QVBoxLayout(playback_window);
  playback_window_layout->setContentsMargins(0, 0, 0, 0);
  playback_window_layout->setSpacing(0);
  const auto renderer_result = create_best_playback_renderer(playback_window);
  auto* playback_canvas_widget = renderer_result.widget;
  auto* playback_window_canvas = renderer_result.canvas;
  playback_canvas_widget->setObjectName("playbackWindowCanvas");
  playback_window_layout->addWidget(playback_canvas_widget, 1);
  auto* task_workspace = new DesktopTaskWorkspace(playback_window, ui_settings);
  auto* playback_window_status = task_workspace->connection_status();
  playback_window_status->setText("The live desktop opens here after the connection is ready.");
  auto* remote_control_button = task_workspace->control_button();
  remote_control_button->setToolTip(
      "Ctrl+Alt+Shift+Esc immediately exits capture. Ctrl+Alt+Del is not supported.");
  auto* remote_control_status = task_workspace->control_status();
  remote_control_status->setText("View only — waiting for Host authorization.");
  auto* retry_capture_button = task_workspace->retry_button();
  auto* playback_control_hint = new PlaybackControlHintOverlay(playback_canvas_widget);
  auto* agent_panel = new AgentConversationPanel(ui_settings);
  task_workspace->add_task(DesktopTask::kAgent, "Agent", agent_panel, QSize(420, 600));
#ifdef _WIN32
  auto* terminal_panel = new TerminalPanel(controller.workspace_pipe());
  task_workspace->add_task(DesktopTask::kTerminal, QString::fromUtf8("终端"), terminal_panel, QSize(760, 320));
  controller.set_workspace_shutdown([weak = QPointer<TerminalPanel>(terminal_panel)](std::function<void()> finished) {
    if (weak) weak->end_desktop(std::move(finished)); else finished();
  });
#endif
  auto* desktop_navigation_panel = new DesktopNavigationPanel();
  task_workspace->add_task(DesktopTask::kNavigation, QString::fromUtf8("桌面导航"), desktop_navigation_panel, QSize(360, 320));
  auto* file_transfer_panel = new FileTransferPanel(
      [&controller](const auto& message, QString* error) { return controller.send_control_message(message, error); });
  task_workspace->add_task(DesktopTask::kFiles, QString::fromUtf8("文件/剪贴板"), file_transfer_panel, QSize(640, 420));
#ifdef _WIN32
  if (gui_auto_start.enable_workspace_control) {
    auto* workspace_control = new WorkspaceControlServer(
        [terminal_panel, file_transfer_panel](const QString& method, const QJsonObject& params, const QString& id) {
          if (method.startsWith("terminal.")) return terminal_panel->coordinator().invoke(method, params, id);
          if (method.startsWith("operation.")) {
            auto result = terminal_panel->coordinator().invoke(method, params, id);
            if (result.value("error").toString() != "operation_not_found" && result.value("error").toString() != "operation_not_running") return result;
          }
          auto result = file_transfer_panel->coordinator().invoke(method, params, id);
          if (method == "status" || method == "capabilities") result["terminal"] = terminal_panel->coordinator().invoke("terminal.status", {});
          return result;
        }, &window);
    const auto observe = [workspace_control](const QString& id, const QJsonObject& state) { workspace_control->observe(id, state); };
    terminal_panel->coordinator().event = observe; file_transfer_panel->coordinator().event = observe;
    QString error;
    if (!workspace_control->start(gui_auto_start.workspace_control_name, &error))
      qWarning("workspace_control_listen_failed");
  }
#endif
  auto* agent_panel_presentation = new AgentPanelPresentation(agent_panel, &window);
  session_layout->addWidget(agent_panel_presentation->open_button());
  agent_panel->set_collapse_callback([agent_panel_presentation, task_workspace, role_combo]() {
    if (role_combo->currentText() == "host") agent_panel_presentation->hide_window();
    else task_workspace->set_task_visible(DesktopTask::kAgent, false);
  });
  std::uint64_t gui_control_message_id = 0;
  std::uint64_t source_activity_revision = 0;
  std::uint64_t source_reference_keyframe_id = 0;
  bool media_budget_waiting = false;
  redclaw::ui::CapturePlaybackState capture_playback_state;
  PlaybackFrameProgress playback_progress;
  auto& latest_displayable_frame_id = playback_progress.displayable;
  auto& latest_displayable_keyframe_id = playback_progress.keyframe;
  auto& latest_presented_frame_id = playback_progress.presented;
  std::uint64_t receiver_decoded_frames = 0;
  std::uint64_t receiver_rendered_frames = 0;
  auto& last_reported_displayable_frame_id = playback_progress.reported;
  bool remote_input_supported = false;
  bool remote_input_authorized = false;
  bool remote_input_frame_ready = false;
  bool remote_input_request_pending = false;
  std::uint64_t pending_capture_geometry_revision = 0;
  std::uint64_t applied_capture_geometry_revision = 0;
  std::uint64_t last_presented_capture_geometry_revision = 0;
  auto remote_input_state = redclaw::protocol::RemoteInputControlStateV1::kUnavailable;
  auto remote_input_reason = redclaw::protocol::RemoteInputStatusReasonV1::kNone;
  qint64 last_remote_input_metrics_log_ms = 0;
  auto send_displayable_stats = [&](bool force) {
    if (source_activity_revision == 0 || latest_displayable_frame_id == 0) {
      return false;
    }
    if (!force && latest_displayable_frame_id == last_reported_displayable_frame_id) {
      return true;
    }
    redclaw::protocol::StreamControlMessageV1 stats;
    stats.type = redclaw::protocol::StreamControlMessageTypeV1::kReceiverNetworkStats;
    stats.session_epoch = "local";
    stats.message_id = ++gui_control_message_id;
    stats.sent_at_ms = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    stats.observed_source_activity_revision = source_activity_revision;
    stats.latest_displayable_frame_id = latest_displayable_frame_id;
    stats.latest_displayable_keyframe_id = latest_displayable_keyframe_id;
    stats.latest_presented_frame_id = latest_presented_frame_id;
    stats.received_frames = receiver_decoded_frames;
    stats.reassembled_frames = receiver_decoded_frames;
    stats.decoded_frames = receiver_decoded_frames;
    stats.rendered_frames = (std::min)(receiver_rendered_frames, receiver_decoded_frames);
    QString send_error;
    if (!controller.send_control_message(stats, &send_error)) {
      return false;
    }
    last_reported_displayable_frame_id = latest_displayable_frame_id;
    return true;
  };
  auto* remote_input_capture = new ControllerRemoteInputCapture(
      playback_canvas_widget, playback_window);
  task_workspace->set_local_interaction_callback([weak = QPointer<ControllerRemoteInputCapture>(remote_input_capture)] {
    if (weak) weak->set_local_suspension(LocalInputSuspensionReason::kLocalUiFocus, true);
  });
#if defined(_WIN32) && !defined(NDEBUG)
  std::unique_ptr<QaInputProbe> qa_input_probe;
  std::unique_ptr<InputDiagnosticTarget> input_diagnostic_target;
  if (gui_auto_start.input_diagnostics && gui_auto_start.role == "host")
    input_diagnostic_target = std::make_unique<InputDiagnosticTarget>();
  if (gui_auto_start.agent_qa_fixture_provider && gui_auto_start.enable_debug_control && gui_auto_start.role == "controller")
    qa_input_probe = std::make_unique<QaInputProbe>(*playback_canvas_widget, *remote_input_capture);
#endif
  remote_input_capture->set_send_message_callback(
      [&controller](const redclaw::protocol::StreamControlMessageV1& message, QString* error) {
        return controller.send_control_message(message, error);
      });
  auto refresh_remote_control_ui = [&]() {
    if (remote_input_capture->active()) {
      remote_control_button->setText("Stop Control");
      remote_control_button->setEnabled(true);
      if (remote_input_capture->input_forwarding()) {
        remote_control_status->setText(
            "Control active — keyboard and mouse are owned by the displayed desktop. Press Ctrl+Alt+Shift+Esc to stop.");
      } else {
        const QString reason = remote_input_capture->local_suspension_reason();
        if (reason.contains("local_ui_focus")) {
          remote_control_status->setText(
              QString("Control remains enabled; the local navigation or Agent panel owns the keyboard. Click the remote desktop to resume input. Waiting on: %1")
                  .arg(reason));
        } else {
          remote_control_status->setText(
              QString("Control remains enabled; input forwarding is paused until the window is active, visible, restored, and geometry is committed. Waiting on: %1")
                  .arg(reason.isEmpty() ? "local_window_state" : reason));
        }
      }
      return;
    }
    remote_control_button->setText("Start Control");
    remote_control_button->setEnabled(
        role_combo->currentText() == "controller"
        && controller.is_running()
        && remote_input_supported
        && remote_input_authorized
        && remote_input_frame_ready
        && !capture_playback_state.waiting()
        && !remote_input_request_pending
        && !file_transfer_panel->busy());
    if (!remote_input_supported) {
      remote_control_status->setText("View only — the peer has not advertised remote input support.");
    } else if (!remote_input_authorized) {
      remote_control_status->setText("View only — Host did not authorize keyboard and mouse control.");
    } else if (!remote_input_frame_ready) {
      remote_control_status->setText("Control available after the first real desktop frame is displayed.");
    } else if (remote_input_request_pending) {
      remote_control_status->setText("Requesting control from Host…");
    } else {
      remote_control_status->setText(
          "Control available. Activate this playback window, then click Start Control; mouse input stays inside the displayed desktop.");
    }
  };
  file_transfer_panel->set_busy_callback([&](bool busy) {
    remote_input_capture->set_local_suspension(LocalInputSuspensionReason::kWorkspaceTransfer, busy);
    desktop_navigation_panel->set_workspace_blocked(busy);
    agent_panel->set_workspace_blocked(busy);
#ifdef _WIN32
    terminal_panel->set_workspace_blocked(busy);
#endif
    refresh_remote_control_ui();
  });
  remote_input_capture->set_clipboard_paste_callback([file_transfer_panel](std::uint32_t sequence) {
    file_transfer_panel->request_clipboard_paste(sequence);
  });
  remote_input_capture->set_paused_callback([&](const QString& reason) {
    file_transfer_panel->cancel_clipboard_paste();
    remote_input_request_pending = false;
    playback_control_hint->hide_hint();
    append_log(session_log, QString("Remote input capture paused: %1").arg(reason));
    refresh_remote_control_ui();
  });
  remote_input_capture->set_forwarding_changed_callback([&]() {
    if (!remote_input_capture->clipboard_paste_context_valid()) file_transfer_panel->cancel_clipboard_paste();
    refresh_remote_control_ui();
  });
  remote_input_capture->set_blocked_click_callback([&]() {
    playback_control_hint->show_hint(
        remote_input_supported && remote_input_authorized && remote_input_frame_ready
        ? QString::fromUtf8("未开始控制，请点击 Start Control")
        : QString::fromUtf8("当前为仅观看模式"));
  });
  auto send_input_control_request = [&](bool active) {
    redclaw::protocol::StreamControlMessageV1 request;
    request.type = redclaw::protocol::StreamControlMessageTypeV1::kInputControlRequest;
    request.session_epoch = "local";
    request.message_id = ++gui_control_message_id;
    request.sent_at_ms = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    request.input_requested_active = active;
    QString send_error;
    if (!controller.send_control_message(request, &send_error)) {
      remote_input_request_pending = false;
      append_log(session_log, QString("Remote input control request failed: %1").arg(send_error));
      refresh_remote_control_ui();
      return false;
    }
    return true;
  };
  QObject::connect(remote_control_button, &QPushButton::clicked, [&]() {
    if (remote_input_capture->active()) {
      remote_input_capture->pause(true, "Paused by Controller user.");
      (void)send_input_control_request(false);
      return;
    }
    if (!capture_playback_state.request_control()) { return; }
    remote_input_request_pending = true;
    if (!send_input_control_request(true)) {
      return;
    }
    refresh_remote_control_ui();
  });
  QObject::connect(retry_capture_button, &QPushButton::clicked, [&]() {
    if (!capture_playback_state.retry_available() || !controller.is_running()) { return; }
    redclaw::protocol::StreamControlMessageV1 request;
    request.type = redclaw::protocol::StreamControlMessageTypeV1::kKeyframeRequest;
    request.session_epoch = "local";
    request.message_id = ++gui_control_message_id;
    request.sent_at_ms = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    request.capture_status_version = 1;
    request.capture_retry_requested = true;
    request.payload = "user_capture_retry";
    QString error;
    if (!controller.send_control_message(request, &error)) {
      append_log(session_log, QString("Capture retry failed: %1").arg(error));
    }
  });
  auto* playback_geometry_controller = new PlaybackWindowGeometryController(
      playback_window, playback_canvas_widget);
  playback_geometry_controller->set_active_changed_callback(
      [playback_window_canvas, remote_input_capture](bool active) {
        playback_window_canvas->set_geometry_transaction_active(active);
        remote_input_capture->set_local_suspension(
            LocalInputSuspensionReason::kGeometryTransaction,
            active);
      });
  playback_geometry_controller->set_viewport_committed_callback(
      [&](QSize physical_canvas_size, std::uint64_t transaction_id) {
        playback_window_canvas->commit_geometry(transaction_id);
        if (role_combo->currentText() != "controller") {
          return;
        }
        redclaw::protocol::StreamControlMessageV1 viewport;
        viewport.type = redclaw::protocol::StreamControlMessageTypeV1::kViewportRequest;
        viewport.session_epoch = "local";
        viewport.message_id = ++gui_control_message_id;
        viewport.sent_at_ms = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
        viewport.viewport_width = static_cast<std::uint32_t>(physical_canvas_size.width());
        viewport.viewport_height = static_cast<std::uint32_t>(physical_canvas_size.height());
        viewport.log_cursor = transaction_id;
        QString send_error;
        if (!controller.send_control_message(viewport, &send_error)) {
          append_log(
              session_log,
              QString("Viewport request queued until runtime is available: %1").arg(send_error));
          return;
        }
        append_log(
            session_log,
            QString("GUI playback geometry commit transaction_id=%1 viewport_width=%2 viewport_height=%3 geometry_preview_total=%4 geometry_commit_total=%5 viewport_request_total=%6 swapchain_resize_total=%7 transitions_disabled=%8")
                .arg(QString::number(transaction_id))
                .arg(QString::number(physical_canvas_size.width()))
                .arg(QString::number(physical_canvas_size.height()))
                .arg(QString::number(playback_geometry_controller->geometry_preview_total()))
                .arg(QString::number(playback_geometry_controller->geometry_commit_total()))
                .arg(QString::number(playback_geometry_controller->viewport_commit_total()))
                .arg(QString::number(playback_window_canvas->swap_chain_resize_count()))
                .arg(playback_geometry_controller->transitions_disabled() ? "true" : "false"));
      });
  playback_geometry_controller->start();

  auto* error_banner = new QLabel("Waiting for you to start a session.", session_page);
  error_banner->setWordWrap(true);
  session_layout->addWidget(error_banner);
  error_banner->setVisible(false);

  auto apply_status_pill = [](QLabel* label, const QString& tone) {
    QString style = "padding: 6px 12px; border-radius: 999px; font: 600 12pt \"Segoe UI\";";
    if (tone == "good") {
      style += "background-color: rgba(5, 150, 105, 0.18); color: #d1fae5; border: 1px solid #10b981;";
    } else if (tone == "warn") {
      style += "background-color: rgba(245, 158, 11, 0.16); color: #fde68a; border: 1px solid #f59e0b;";
    } else if (tone == "bad") {
      style += "background-color: rgba(239, 68, 68, 0.18); color: #fecaca; border: 1px solid #ef4444;";
    } else {
      style += "background-color: rgba(37, 99, 235, 0.16); color: #bfdbfe; border: 1px solid #3b82f6;";
    }
    if (label->styleSheet() != style) label->setStyleSheet(style);
  };

  auto set_runtime_banner = [&](const QString& text, const QString& tone) {
    QString style = "padding: 10px 14px; border-radius: 14px; border: 1px solid #27446b; background-color: rgba(19, 34, 56, 0.82); color: #dbeafe;";
    if (tone == "good") {
      style = "padding: 10px 14px; border-radius: 14px; border: 1px solid #10b981; background-color: rgba(6, 95, 70, 0.34); color: #d1fae5;";
    } else if (tone == "warn") {
      style = "padding: 10px 14px; border-radius: 14px; border: 1px solid #f59e0b; background-color: rgba(120, 53, 15, 0.34); color: #fde68a;";
    } else if (tone == "bad") {
      style = "padding: 10px 14px; border-radius: 14px; border: 1px solid #ef4444; background-color: rgba(127, 29, 29, 0.42); color: #fee2e2;";
    }
    error_banner->setText(text);
    if (error_banner->styleSheet() != style) error_banner->setStyleSheet(style);
  };

  auto set_connection_state_display = [&](const QString& state) {
    const QString lowered = state.trimmed().toLower();
    QString text = "Ready";
    QString tone = "info";
    if (lowered == "connected" || lowered == "completed") {
      text = "Connected";
      tone = "good";
    } else if (lowered == "checking" || lowered == "new" || lowered == "connecting") {
      text = "Connecting";
      tone = "warn";
    } else if (lowered == "failed" || lowered == "disconnected") {
      text = "Needs attention";
      tone = "bad";
    } else if (lowered == "closed") {
      text = "Closed";
    }
    connection_state_value->setText(text);
    apply_status_pill(connection_state_value, tone);
  };

  auto set_handshake_state = [&](bool ready) {
    remote_desc_value->setText(ready ? "Ready" : "Waiting");
    apply_status_pill(remote_desc_value, ready ? "good" : "warn");
  };

  auto set_live_link_state = [&](bool connected) {
    connected_value->setText(connected ? "Live" : "Pending");
    apply_status_pill(connected_value, connected ? "good" : "warn");
  };

  auto set_process_state = [&](const QString& state) {
    const QString lowered = state.trimmed().toLower();
    QString text = "Idle";
    QString tone = "info";
    if (lowered == "running") {
      text = "Active";
      tone = "good";
    } else if (lowered == "stopping") {
      text = "Stopping";
      tone = "warn";
    } else if (lowered == "failed" || lowered == "crashed") {
      text = "Needs attention";
      tone = "bad";
    }
    process_mode_value->setText(text);
    apply_status_pill(process_mode_value, tone);
  };

  set_connection_state_display("idle");
  set_handshake_state(false);
  set_live_link_state(false);
  set_process_state("idle");
  set_runtime_banner("Waiting for you to start a session.", "info");

  auto set_link_workflow_running = [&](bool running, const QString& title = QString()) {
    Q_UNUSED(title);
    connection_entry_page->set_actions_enabled(!running);
    stun_preset_combo->setEnabled(!running);
    additional_ice_servers->setEnabled(!running);
    link_network_exit->setEnabled(!running);
    link_ice_udp_port->setEnabled(!running);
    link_enable_port_mapping->setEnabled(!running);
  };

  auto* runtime_detail_tabs = new QTabWidget(runtime_page);
  runtime_detail_tabs->setDocumentMode(true);

  auto* timeline_list = new QListWidget(runtime_page);
  timeline_list->setMinimumHeight(140);
  runtime_detail_tabs->addTab(timeline_list, "Timeline");

  auto* runtime_log = new RuntimeLogView(runtime_page);
  runtime_log->setDocument(session_log->document());
  runtime_log->setPlaceholderText("Runtime output mirror");
  runtime_log->setMinimumHeight(140);
  runtime_detail_tabs->addTab(runtime_log, "Runtime Log");

  auto* remote_log_page = new QWidget(runtime_page);
  auto* remote_log_layout = new QVBoxLayout(remote_log_page);
  remote_log_layout->setContentsMargins(0, 0, 0, 0);
  auto* remote_log_actions = new QHBoxLayout();
  auto* remote_log_snapshot_button = new QPushButton("Request snapshot", remote_log_page);
  auto* remote_log_follow_button = new QPushButton("Start follow", remote_log_page);
  auto* remote_log_stop_button = new QPushButton("Stop follow", remote_log_page);
  remote_log_actions->addWidget(remote_log_snapshot_button);
  remote_log_actions->addWidget(remote_log_follow_button);
  remote_log_actions->addWidget(remote_log_stop_button);
  remote_log_actions->addStretch(1);
  auto* remote_log = new QPlainTextEdit(remote_log_page);
  remote_log->setReadOnly(true);
  remote_log->setPlaceholderText("Redacted peer logs will appear here on request.");
  remote_log_layout->addLayout(remote_log_actions);
  remote_log_layout->addWidget(remote_log, 1);
  runtime_detail_tabs->addTab(remote_log_page, "Remote Log");
  QString remote_log_follow_request_id;
  std::uint64_t remote_log_cursor = 0;
  DebugRemoteLogSnapshot debug_remote_log_snapshot;

  std::uint64_t gui_agent_message_id = 0;
  std::unique_ptr<AgentControlServer> agent_control_server;
  auto dispatch_agent_command = [&](redclaw::protocol::AgentMessageEnvelopeV1 message,
                                QString* error_detail = nullptr) {
    message.session_epoch = "local-gui";
    message.message_id = ++gui_agent_message_id;
    message.sent_at_ms = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    std::string validation_error;
    if (!redclaw::protocol::validate_agent_message_v1(message, &validation_error)) {
      if (error_detail != nullptr) {
        *error_detail = QString::fromStdString(validation_error);
      }
      return false;
    }
    return controller.send_agent_message(message, error_detail);
  };
  {
    AgentControlServerConfig agent_control_config;
    agent_control_config.listen_external = gui_auto_start.enable_agent_control;
    agent_control_config.pipe_name = gui_auto_start.agent_control_name;
    if (!gui_auto_start.coordination_journal_path.isEmpty()) {
      agent_control_config.journal_path = std::filesystem::path(
          gui_auto_start.coordination_journal_path.toStdWString());
    }
    agent_control_config.git_sha = gui_auto_start.coordination_git_sha.toStdString();
    agent_control_config.executable_sha256 = sha256_file_hex(
        QCoreApplication::applicationFilePath()).toStdString();
    agent_control_server = std::make_unique<AgentControlServer>(
        std::move(agent_control_config),
        [&](redclaw::protocol::AgentMessageEnvelopeV1 message, QString* send_error) {
          return dispatch_agent_command(std::move(message), send_error);
        },
        [&](const QString& line) { append_log(session_log, line); },
        &window);
    QString agent_control_error;
    if (!agent_control_server->start(&agent_control_error)) {
      append_log(session_log, agent_control_error + "; desktop remains available");
    }
  }

  auto send_agent_command = [&](redclaw::protocol::AgentMessageEnvelopeV1 message,
                                QString* error_detail = nullptr) {
    message.session_epoch = "local-gui";
    message.message_id = ++gui_agent_message_id;
    message.sent_at_ms = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    return agent_control_server->submit(std::move(message), error_detail);
  };
  auto make_agent_command = [&](redclaw::protocol::AgentMessageTypeV1 type) {
    auto message = make_gui_agent_command(type);
    message.task_id = agent_panel->current_task_id().toStdString();
    message.provider = agent_panel->selected_provider();
    message.model = agent_panel->selected_model().toStdString();
    message.project_id = agent_panel->selected_project_id().toStdString();
    message.work_directory_mode = agent_panel->selected_work_directory_mode();
    return message;
  };
  agent_panel->set_submit_callback([&](AgentSubmitMode mode, const QString& instruction) {
    const auto type = mode == AgentSubmitMode::kCreateTask
        ? redclaw::protocol::AgentMessageTypeV1::kTaskCreate
        : mode == AgentSubmitMode::kSteerTurn
            ? redclaw::protocol::AgentMessageTypeV1::kTurnSteer
            : redclaw::protocol::AgentMessageTypeV1::kTurnStart;
    QString task_id = agent_panel->current_task_id();
    if (mode == AgentSubmitMode::kCreateTask) {
      task_id = "task-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    auto message = make_agent_command(type);
    message.task_id = task_id.toStdString();
    message.text = instruction.toUtf8().toStdString();
    QString send_error;
    if (!send_agent_command(std::move(message), &send_error)) {
      return AgentSubmitResult{.ok = false, .error = send_error};
    }
    return AgentSubmitResult{.ok = true, .task_id = task_id};
  });
  agent_panel->set_interrupt_callback([&](QString* send_error) {
    return send_agent_command(
        make_agent_command(redclaw::protocol::AgentMessageTypeV1::kTurnInterrupt),
        send_error);
  });
  agent_panel->set_approval_callback(
      [&](redclaw::protocol::AgentApprovalDecisionV1 decision, QString* send_error) {
        auto message = make_agent_command(
            redclaw::protocol::AgentMessageTypeV1::kApprovalDecision);
        message.request_id = agent_panel->approval_request_id().toStdString();
        message.approval_decision = decision;
        return send_agent_command(std::move(message), send_error);
      });
  agent_panel_presentation->set_desktop_host(role_combo->currentText() == "host");
  task_workspace->set_enabled(role_combo->currentText() != "host");
  QObject::connect(role_combo, &QComboBox::currentTextChanged, &window,
      [agent_panel_presentation, task_workspace](const QString& role) {
        agent_panel_presentation->set_desktop_host(role == "host");
        task_workspace->set_enabled(role != "host");
      });

  auto send_remote_log_request = [&](
      redclaw::protocol::RemoteLogModeV1 mode,
      QString* peer_request_id,
      QString* error_detail) {
    const auto assign_send_error = [&](const QString& detail) {
      if (error_detail != nullptr) {
        *error_detail = detail;
      }
    };
    if (role_combo->currentText() != "controller") {
      const QString detail = "Remote log requests are initiated from the Controller endpoint.";
      append_log(session_log, detail);
      assign_send_error(detail);
      return false;
    }
    redclaw::protocol::StreamControlMessageV1 request;
    request.type = redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogRequest;
    request.session_epoch = "local";
    request.message_id = ++gui_control_message_id;
    request.sent_at_ms = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    request.log_mode = mode;
    if (mode == redclaw::protocol::RemoteLogModeV1::kSnapshot) {
      request.request_id = QString("snapshot-%1").arg(QString::number(request.sent_at_ms)).toStdString();
      request.log_cursor = 0;
    } else {
      if (remote_log_follow_request_id.isEmpty()) {
        remote_log_follow_request_id = QString("follow-%1").arg(QString::number(request.sent_at_ms));
      }
      request.request_id = remote_log_follow_request_id.toStdString();
      request.log_cursor = remote_log_cursor;
    }
    const QString request_id = QString::fromStdString(request.request_id);
    if (peer_request_id != nullptr) {
      *peer_request_id = request_id;
    }
    if (mode == redclaw::protocol::RemoteLogModeV1::kSnapshot) {
      debug_remote_log_snapshot.begin(request_id);
    }
    QString send_error;
    if (!controller.send_control_message(request, &send_error)) {
      append_log(session_log, QString("Remote log request failed: %1").arg(send_error));
      if (mode == redclaw::protocol::RemoteLogModeV1::kSnapshot) {
        debug_remote_log_snapshot.fail(request_id, send_error);
      }
      assign_send_error(send_error);
      return false;
    }
    if (mode == redclaw::protocol::RemoteLogModeV1::kSnapshot) {
      remote_log->appendPlainText("--- requested remote log snapshot ---");
    } else if (mode == redclaw::protocol::RemoteLogModeV1::kFollow) {
      remote_log->appendPlainText("--- remote log follow started ---");
    } else {
      remote_log->appendPlainText("--- remote log follow stopped ---");
      remote_log_follow_request_id.clear();
    }
    runtime_detail_tabs->setVisible(true);
    runtime_detail_tabs->setCurrentWidget(remote_log_page);
    if (error_detail != nullptr) {
      error_detail->clear();
    }
    return true;
  };

  QObject::connect(remote_log_snapshot_button, &QPushButton::clicked, [&]() {
    (void)send_remote_log_request(redclaw::protocol::RemoteLogModeV1::kSnapshot, nullptr, nullptr);
  });
  QObject::connect(remote_log_follow_button, &QPushButton::clicked, [&]() {
    (void)send_remote_log_request(redclaw::protocol::RemoteLogModeV1::kFollow, nullptr, nullptr);
  });
  QObject::connect(remote_log_stop_button, &QPushButton::clicked, [&]() {
    (void)send_remote_log_request(redclaw::protocol::RemoteLogModeV1::kStop, nullptr, nullptr);
  });
  runtime_detail_tabs->setVisible(false);

  QObject::connect(copy_session_log_button, &QPushButton::clicked, [&]() {
    QString combined = session_log->toPlainText().trimmed();
    const QString runtime_text = runtime_log->toPlainText().trimmed();
    if (!runtime_text.isEmpty()) {
      if (combined.isEmpty()) {
        combined = runtime_text;
      } else if (!combined.contains(runtime_text)) {
        combined += "\n\n--- runtime mirror ---\n";
        combined += runtime_text;
      }
    }
    if (combined.isEmpty()) {
      session_log_copy_feedback->setText("Log is empty");
      set_link_status("Log is empty; nothing to copy yet.", "warn");
      return;
    }
    QGuiApplication::clipboard()->setText(combined);
    session_log_copy_feedback->setText("Copied to clipboard");
    set_link_status("Runtime log copied to clipboard.", "good");
    QTimer::singleShot(2500, session_log_copy_feedback, [session_log_copy_feedback]() {
      session_log_copy_feedback->clear();
    });
  });

  QObject::connect(clear_session_log_button, &QPushButton::clicked, [&]() {
    session_log->clear();
    session_log_copy_feedback->setText("Log cleared");
    QTimer::singleShot(2000, session_log_copy_feedback, [session_log_copy_feedback]() {
      session_log_copy_feedback->clear();
    });
  });

  redclaw::render::RuntimeStatusTimeline runtime_timeline;
  redclaw::render::RuntimeStatusTimelineUiModel runtime_timeline_ui_model;
  runtime_timeline_ui_model.bind(&runtime_timeline);
  redclaw::render::RuntimeStatusTimelineWidgetComponent runtime_timeline_widget_component;
  runtime_timeline_widget_component.bind(&runtime_timeline_ui_model);

  auto* diag_page = new QWidget();
  auto* diag_layout = new QVBoxLayout(diag_page);
  diag_layout->setContentsMargins(0, 0, 0, 0);
  diag_layout->setSpacing(12);

  auto* diag_summary = new QLabel(
      "Use this page when pairing needs evidence: ICE, DHT, NAT, stream counters, and service management stay available without crowding the primary workflow.",
      diag_page);
  diag_summary->setWordWrap(true);
  diag_summary->setObjectName("pageIntro");
  diag_layout->addWidget(diag_summary);

  auto* diag_metrics = new QGroupBox("Health Signals", diag_page);
  auto* diag_metrics_grid = new QGridLayout(diag_metrics);
  diag_metrics_grid->setHorizontalSpacing(18);
  diag_metrics_grid->setVerticalSpacing(8);
  auto* ice_config_value = new QLabel("n/a", diag_metrics);
  auto* candidate_stats_value = new QLabel("n/a", diag_metrics);
  auto* rendezvous_stats_value = new QLabel("n/a", diag_metrics);
  auto* dht_stats_value = new QLabel("n/a", diag_metrics);
  auto* nat_diagnostics_value = new QLabel("n/a", diag_metrics);
  auto* tcp_stats_value = new QLabel("n/a", diag_metrics);
  auto* stream_stats_value = new QLabel("n/a", diag_metrics);
  auto* last_failure_value = new QLabel("none", diag_metrics);

  ice_config_value->setWordWrap(true);
  candidate_stats_value->setWordWrap(true);
  rendezvous_stats_value->setWordWrap(true);
  dht_stats_value->setWordWrap(true);
  nat_diagnostics_value->setWordWrap(true);
  tcp_stats_value->setWordWrap(true);
  stream_stats_value->setWordWrap(true);
  last_failure_value->setWordWrap(true);

  diag_metrics_grid->addWidget(new QLabel("ICE config", diag_metrics), 0, 0);
  diag_metrics_grid->addWidget(ice_config_value, 0, 1);
  diag_metrics_grid->addWidget(new QLabel("Candidate stats", diag_metrics), 1, 0);
  diag_metrics_grid->addWidget(candidate_stats_value, 1, 1);
  diag_metrics_grid->addWidget(new QLabel("Rendezvous stats", diag_metrics), 2, 0);
  diag_metrics_grid->addWidget(rendezvous_stats_value, 2, 1);
  diag_metrics_grid->addWidget(new QLabel("DHT stats", diag_metrics), 3, 0);
  diag_metrics_grid->addWidget(dht_stats_value, 3, 1);
  diag_metrics_grid->addWidget(new QLabel("NAT diagnostics", diag_metrics), 4, 0);
  diag_metrics_grid->addWidget(nat_diagnostics_value, 4, 1);
  diag_metrics_grid->addWidget(new QLabel("TCP stats", diag_metrics), 5, 0);
  diag_metrics_grid->addWidget(tcp_stats_value, 5, 1);
  diag_metrics_grid->addWidget(new QLabel("Stream stats", diag_metrics), 6, 0);
  diag_metrics_grid->addWidget(stream_stats_value, 6, 1);
  diag_metrics_grid->addWidget(new QLabel("Last failure", diag_metrics), 7, 0);
  diag_metrics_grid->addWidget(last_failure_value, 7, 1);

  diag_layout->addWidget(diag_metrics);

  auto* diag_notes_box = new QGroupBox("Diagnostic Notes", diag_page);
  auto* diag_notes_layout = new QVBoxLayout(diag_notes_box);
  auto* diag_text = new QPlainTextEdit(diag_notes_box);
  diag_text->setReadOnly(true);
  diag_text->setPlainText("- Service lifecycle panel: added\n- Elevated action UX: added\n- Installer runtime checks: pending\n");
  diag_text->setMaximumHeight(120);
  diag_notes_layout->addWidget(diag_text);
  diag_layout->addWidget(diag_notes_box);

  auto* service_box = new QGroupBox("Service Management", diag_page);
  auto* service_form = new QFormLayout(service_box);

  auto* service_name = new QLineEdit(service_box);
  service_name->setText("RedClawHostService");

  auto* service_display = new QLineEdit(service_box);
  service_display->setText("RedClaw Host Service");

  auto* service_binary = new QLineEdit(service_box);
  service_binary->setText(QCoreApplication::applicationFilePath());

  auto* service_auto_start = new QCheckBox("Auto start", service_box);
  service_auto_start->setChecked(true);

  auto* service_account = new QLineEdit(service_box);
  service_account->setText("LocalSystem");

  auto* service_env = new QPlainTextEdit(service_box);
  service_env->setPlaceholderText("Optional env overrides, one KEY=VALUE per line");
  service_env->setMaximumHeight(70);

  auto* service_status = new QLabel("Service not acted on yet", service_box);
  auto* service_last_error = new QLabel("none", service_box);
  service_last_error->setWordWrap(true);

  service_form->addRow("Service name", service_name);
  service_form->addRow("Display name", service_display);
  service_form->addRow("Binary path", service_binary);
  service_form->addRow("Account", service_account);
  service_form->addRow("Environment overrides", service_env);
  service_form->addRow("Auto start", service_auto_start);
  service_form->addRow("Status", service_status);
  service_form->addRow("Last error", service_last_error);

  auto* service_button_row = new QHBoxLayout();
  auto* service_install_button = new QPushButton("Install", service_box);
  auto* service_start_button = new QPushButton("Start", service_box);
  auto* service_stop_button = new QPushButton("Stop", service_box);
  auto* service_uninstall_button = new QPushButton("Uninstall", service_box);
  service_button_row->addWidget(service_install_button);
  service_button_row->addWidget(service_start_button);
  service_button_row->addWidget(service_stop_button);
  service_button_row->addWidget(service_uninstall_button);
  service_form->addRow("Actions", service_button_row);

  auto* service_note = new QLabel("Install/Start/Stop/Uninstall typically require an elevated shell.", service_box);
  service_note->setWordWrap(true);
  service_form->addRow("Note", service_note);

  auto* service_readiness_box = new QGroupBox("Install Readiness", service_box);
  auto* service_readiness_grid = new QGridLayout(service_readiness_box);
  auto* service_binary_ready_value = new QLabel("unknown", service_readiness_box);
  auto* service_installed_value = new QLabel("unknown", service_readiness_box);
  auto* service_running_value = new QLabel("unknown", service_readiness_box);
  auto* service_readiness_summary = new QLabel("Pending check", service_readiness_box);
  service_readiness_summary->setWordWrap(true);

  service_readiness_grid->addWidget(new QLabel("Binary exists", service_readiness_box), 0, 0);
  service_readiness_grid->addWidget(service_binary_ready_value, 0, 1);
  service_readiness_grid->addWidget(new QLabel("Service installed", service_readiness_box), 1, 0);
  service_readiness_grid->addWidget(service_installed_value, 1, 1);
  service_readiness_grid->addWidget(new QLabel("Service running", service_readiness_box), 2, 0);
  service_readiness_grid->addWidget(service_running_value, 2, 1);
  service_readiness_grid->addWidget(new QLabel("Summary", service_readiness_box), 3, 0);
  service_readiness_grid->addWidget(service_readiness_summary, 3, 1);
  service_form->addRow(service_readiness_box);

  auto* service_check_button = new QPushButton("Run Readiness Check", service_box);
  service_form->addRow("Validation", service_check_button);

  redclaw::service::WindowsServiceLifecycleWrapper service_wrapper;

  auto set_service_feedback = [&](const QString& text, bool is_error) {
    service_status->setText(text);
    if (is_error) {
      service_last_error->setText(text);
      service_note->setStyleSheet("padding: 4px; color: #fee2e2;");
    } else {
      service_note->setStyleSheet("padding: 4px; color: #d1fae5;");
    }
  };

  auto refresh_service_readiness = [&]() {
    const ServiceReadinessState state = inspect_service_readiness(service_binary->text(), service_name->text());
    service_binary_ready_value->setText(state.binary_exists);
    service_installed_value->setText(state.service_exists);
    service_running_value->setText(state.service_running);
    service_readiness_summary->setText(state.summary);

    const bool ready = state.binary_exists == "yes" && state.service_exists != "invalid service name";
    service_install_button->setEnabled(ready);
    service_start_button->setEnabled(state.service_exists == "yes");
    service_stop_button->setEnabled(state.service_exists == "yes");
    service_uninstall_button->setEnabled(state.service_exists == "yes");

    if (state.binary_exists != "yes") {
      service_note->setStyleSheet("padding: 4px; color: #fee2e2;");
      service_note->setText("Install/Start/Stop/Uninstall require a valid host binary path and an elevated shell.");
    }
  };

  auto refresh_runtime_timeline_view = [&]() {
    runtime_timeline_ui_model.refresh();
    runtime_timeline_widget_component.refresh();

    timeline_list->clear();
    const auto& state = runtime_timeline_widget_component.state();
    if (state.items.empty()) {
      timeline_list->addItem("Timeline empty");
      return;
    }

    for (const auto& item : state.items) {
      const QString timestamp = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(item.timestamp_ms))
                                    .toString("yyyy-MM-dd HH:mm:ss");
      QString severity = "INFO";
      if (item.severity == redclaw::render::RuntimeStatusSeverity::kWarning) {
        severity = "WARN";
      } else if (item.severity == redclaw::render::RuntimeStatusSeverity::kError) {
        severity = "ERROR";
      }

      timeline_list->addItem(QString("%1 [%2] %3 - %4")
                                 .arg(timestamp, severity, QString::fromStdString(item.title), QString::fromStdString(item.subtitle)));
    }
  };

  auto append_runtime_event = [&](redclaw::render::RuntimeStatusSeverity severity,
                                  std::string category,
                                  std::string message) {
    redclaw::render::RuntimeStatusEvent event;
    event.timestamp_ms = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    event.severity = severity;
    event.category = std::move(category);
    event.message = std::move(message);
    runtime_timeline.append(std::move(event));
    refresh_runtime_timeline_view();
  };

  QObject::connect(service_check_button, &QPushButton::clicked, [&]() {
    refresh_service_readiness();
    const QString summary = QString("readiness binary=%1 installed=%2 running=%3 summary=%4")
                                .arg(service_binary_ready_value->text(), service_installed_value->text(), service_running_value->text(), service_readiness_summary->text());
    set_service_feedback(summary, false);
    append_log(session_log, QString("Service readiness: %1").arg(summary));
    append_log(diag_text, QString("Service readiness: %1").arg(summary));
    append_runtime_event(redclaw::render::RuntimeStatusSeverity::kInfo, "service-readiness", summary.toStdString());
  });

  auto set_sealed_exchange_state = [&](SealedExchangeState state, const QString& detail = QString()) {
    sealed_exchange_status->setText(sealed_exchange_state_text(state, detail));

    redclaw::render::RuntimeStatusSeverity severity = redclaw::render::RuntimeStatusSeverity::kInfo;
    if (state == SealedExchangeState::kFailed) {
      severity = redclaw::render::RuntimeStatusSeverity::kError;
    }
    append_runtime_event(severity, "sealed-exchange", sealed_exchange_status->text().toStdString());
  };

  refresh_runtime_timeline_view();

  auto collect_env_overrides = [&]() {
    std::unordered_map<std::string, std::string> overrides;
    const QStringList lines = service_env->toPlainText().split('\n');
    for (const QString& line : lines) {
      const QString trimmed = line.trimmed();
      if (trimmed.isEmpty() || !trimmed.contains('=')) {
        continue;
      }
      const int split = trimmed.indexOf('=');
      const QString key = trimmed.left(split).trimmed();
      const QString value = trimmed.mid(split + 1).trimmed();
      if (!key.isEmpty()) {
        overrides.emplace(key.toStdString(), value.toStdString());
      }
    }
    return overrides;
  };

  QObject::connect(service_name, &QLineEdit::textChanged, [&]() { refresh_service_readiness(); });
  QObject::connect(service_binary, &QLineEdit::textChanged, [&]() { refresh_service_readiness(); });
  refresh_service_readiness();

  QObject::connect(service_install_button, &QPushButton::clicked, [&]() {
    redclaw::service::ServiceInstallConfig config;
    config.service_name = service_name->text().trimmed().toStdString();
    config.display_name = service_display->text().trimmed().toStdString();
    config.binary_path = QFileInfo(service_binary->text().trimmed()).absoluteFilePath().toStdString();
    config.auto_start = service_auto_start->isChecked();
    config.account_name = service_account->text().trimmed().toStdString();
    config.environment_overrides = collect_env_overrides();

    const auto result = service_wrapper.install(config);
    const QString message = QString("install=%1 detail=%2")
        .arg(service_lifecycle_error_to_text(result), QString::fromStdString(service_wrapper.last_error_detail()));
    set_service_feedback(message, result != redclaw::service::ServiceLifecycleError::kNone);
    append_log(session_log, QString("Service install: %1").arg(message));
    append_log(diag_text, QString("Service install: %1").arg(message));
  });

  QObject::connect(service_start_button, &QPushButton::clicked, [&]() {
    const QString name = service_name->text().trimmed();
    const auto result = service_wrapper.start(name.toStdString());
    const QString message = QString("start=%1 detail=%2")
        .arg(service_lifecycle_error_to_text(result), QString::fromStdString(service_wrapper.last_error_detail()));
    set_service_feedback(message, result != redclaw::service::ServiceLifecycleError::kNone);
    append_log(session_log, QString("Service start: %1").arg(message));
  });

  QObject::connect(service_stop_button, &QPushButton::clicked, [&]() {
    const QString name = service_name->text().trimmed();
    const auto result = service_wrapper.stop(name.toStdString());
    const QString message = QString("stop=%1 detail=%2")
        .arg(service_lifecycle_error_to_text(result), QString::fromStdString(service_wrapper.last_error_detail()));
    set_service_feedback(message, result != redclaw::service::ServiceLifecycleError::kNone);
    append_log(session_log, QString("Service stop: %1").arg(message));
  });

  QObject::connect(service_uninstall_button, &QPushButton::clicked, [&]() {
    const QString name = service_name->text().trimmed();
    const auto result = service_wrapper.uninstall(name.toStdString());
    const QString message = QString("uninstall=%1 detail=%2")
        .arg(service_lifecycle_error_to_text(result), QString::fromStdString(service_wrapper.last_error_detail()));
    set_service_feedback(message, result != redclaw::service::ServiceLifecycleError::kNone);
    append_log(session_log, QString("Service uninstall: %1").arg(message));
  });

  service_box->setVisible(false);

  auto* service_toggle = new QToolButton(diag_page);
  service_toggle->setCheckable(true);
  service_toggle->setChecked(false);
  service_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  service_toggle->setArrowType(Qt::RightArrow);
  service_toggle->setText("Show service management tools");
  QObject::connect(service_toggle, &QToolButton::toggled, diag_page, [service_box, service_toggle](bool expanded) {
    service_box->setVisible(expanded);
    service_toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    service_toggle->setText(expanded ? "Hide service management tools" : "Show service management tools");
  });
  diag_layout->addWidget(service_toggle);
  diag_layout->addWidget(service_box);

  auto* diag_scroll = new QScrollArea(tabs);
  diag_scroll->setWidgetResizable(true);
  diag_scroll->setFrameShape(QFrame::NoFrame);
  diag_scroll->setWidget(diag_page);
  diag_scroll->setVisible(false);

  auto generate_local_link_code = [&]() {
    const QString code = generate_machine_link_code();
    connection_entry_page->set_local_code(code);
    session_code->setText(code);
    rendezvous_url->setText(rendezvous_url->text().trimmed());
    set_link_status("Your code is ready. Press Wait with This Code when this device should accept a connection.", "info");
    append_log(session_log, QString("Generated local machine link code: %1").arg(code));
    return true;
  };

  const bool integration_persist_host_wait = gui_auto_start.persist_host_wait;
  bool manual_runtime_stop_requested = false;
  bool debug_reconnect_requested = false;

  auto apply_peer_link_code_to_form = [&]() {
    const QString peer_code_text = connection_entry_page->peer_code();
    if (peer_code_text.isEmpty()) {
      set_link_status("Paste the 8-character code from the other device first.", "bad");
      append_log(session_log, "Peer machine link code rejected: missing input");
      peer_machine_code->setFocus();
      return false;
    }

    MachineLinkCode peer;
    QString decode_error;
    if (!decode_machine_link_code(connection_entry_page->peer_code(), &peer, &decode_error)) {
      set_link_status("This code is not complete yet. Paste the full 8-character code and try again.", "bad");
      append_log(session_log, QString("Peer machine link code rejected: %1").arg(decode_error));
      peer_machine_code->setFocus();
      return false;
    }

    connection_entry_page->set_peer_code(peer.session_code);
    session_code->setText(peer.session_code);
    set_link_status("Code accepted. Starting the connection.", "good");
    append_log(session_log, QString("Decoded peer machine link code: %1").arg(peer.session_code));
    return true;
  };

  auto ensure_runtime_idle_for_link_action = [&](const QString& action_name) {
    if (!controller.is_running()) {
      return true;
    }

    const QString message = QString("A session is already running. Stop it before %1.").arg(action_name);
    set_link_status(message, "bad");
    set_runtime_banner(message, "bad");
    append_log(session_log, QString("Machine link workflow blocked: %1").arg(message));
    tabs->setCurrentWidget(session_scroll);
    return false;
  };

  QObject::connect(generate_local_code_button, &QPushButton::clicked, [&]() {
    if (!ensure_runtime_idle_for_link_action("waiting with this code")) {
      return;
    }

    MachineLinkCode local;
    QString decode_error;
    if (!decode_machine_link_code(connection_entry_page->local_code(), &local, &decode_error)) {
      if (!generate_local_link_code()) {
        return;
      }
      if (!decode_machine_link_code(connection_entry_page->local_code(), &local, &decode_error)) {
        set_link_status("A fresh code could not be created. Please try again.", "bad");
        append_log(session_log, QString("Local machine link code rejected: %1").arg(decode_error));
        return;
      }
    } else {
      connection_entry_page->set_local_code(local.session_code);
      session_code->setText(local.session_code);
      append_log(session_log, QString("Reusing local machine link code: %1").arg(local.session_code));
    }

    role_combo->setCurrentText("host");
    host_wait_remote_input_authorized =
        connection_entry_page->allow_remote_control_checkbox()->isChecked();
    host_wait_remote_agent_authorized =
        connection_entry_page->allow_remote_agent_checkbox()->isChecked();
    if (host_wait_remote_agent_authorized
        && ((!gui_auto_start.agent_qa_fixture_provider && !agent_settings_dialog->any_provider_ready())
            || agent_settings_dialog->project_roots().isEmpty())) {
      set_link_status(
          "Development Agent access requires one ready local provider and one registered project.",
          "bad");
      agent_settings_dialog->set_runtime_active(false);
      agent_settings_dialog->exec();
      return;
    }
    transport_combo->setCurrentText("dht");
    target_host->clear();
    target_port->setValue(45909);
    session_code->setText(local.session_code);
    signal_dir->setText("runtime-signaling-ui-dht-host");
    runtime_config_path->clear();
    runtime_form_override->setChecked(true);
    signal_timeout->setValue(0);
    stream_smoke->setChecked(true);
    stream_require_capture->setChecked(true);
    sync_link_network_settings_to_session_form();
    set_link_status("Starting host waiting mode with the code shown above.", "warn");
    append_log(session_log, QString("Machine link workflow: host waiting through DHT with code %1").arg(local.session_code));
    connection_flow_model.start(ConnectionFlowRole::kHost);
    waiting_page->set_connection_code(local.session_code);
    append_log(
        session_log,
        host_wait_remote_input_authorized
            ? "Host wait authorization: ordinary desktop keyboard and mouse control allowed."
            : "Host wait authorization: view-only; remote input denied.");
    append_log(
        session_log,
        host_wait_remote_agent_authorized
            ? "Host wait authorization: registered development Agent access allowed."
            : "Host wait authorization: development Agent access denied.");
    refresh_connection_flow_page();
    start_button->click();
  });

  QObject::connect(connect_peer_button, &QPushButton::clicked, [&]() {
    if (!ensure_runtime_idle_for_link_action("connecting to another device")) {
      return;
    }

    if (!apply_peer_link_code_to_form()) {
      return;
    }

    role_combo->setCurrentText("controller");
    host_wait_remote_input_authorized = false;
    host_wait_remote_agent_authorized = false;
    transport_combo->setCurrentText("dht");
    signal_dir->setText("runtime-signaling-ui-dht-controller");
    runtime_config_path->clear();
    runtime_form_override->setChecked(true);
    signal_timeout->setValue(0);
    stream_smoke->setChecked(true);
    stream_require_capture->setChecked(false);
    sync_link_network_settings_to_session_form();
    set_link_status("Starting the connection. Make sure the other device is waiting with the code you entered.", "warn");
    append_log(session_log, QString("Machine link workflow: connecting through DHT with peer code %1").arg(session_code->text().trimmed()));
    connection_flow_model.start(ConnectionFlowRole::kController);
    connecting_page->set_connection_code(session_code->text().trimmed());
    refresh_connection_flow_page();
    start_button->click();
  });

  QObject::connect(runtime_config_browse, &QPushButton::clicked, [&]() {
    const QString selected_path = QFileDialog::getOpenFileName(
        &window,
        "Select Runtime Profile",
        runtime_config_path->text().trimmed(),
        "Runtime Profile (*.conf *.txt *.ini);;All Files (*.*)");
    if (!selected_path.isEmpty()) {
      runtime_config_path->setText(selected_path);
    }
  });

  auto local_sealed_blob_path = [&]() {
    const QString dir = signal_dir->text().trimmed();
    if (dir.isEmpty()) {
      return QString();
    }
    const QString filename = role_combo->currentText() == "host"
        ? "host-offer.sealed.txt"
        : "controller-answer.sealed.txt";
    return QDir(dir).filePath(filename);
  };

  auto remote_sealed_blob_path = [&]() {
    const QString dir = signal_dir->text().trimmed();
    if (dir.isEmpty()) {
      return QString();
    }
    const QString filename = role_combo->currentText() == "host"
        ? "controller-answer.sealed.txt"
        : "host-offer.sealed.txt";
    return QDir(dir).filePath(filename);
  };

  auto refresh_local_sealed_blob = [&]() {
    if (transport_combo->currentText() != "sealed-file") {
      return;
    }

    const QString path = local_sealed_blob_path();
    if (path.isEmpty()) {
      return;
    }

    QFile local_file(path);
    if (!local_file.exists()) {
      return;
    }

    if (!local_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      return;
    }

    const QString content = QString::fromUtf8(local_file.readAll()).trimmed();
    if (!content.isEmpty() && content != local_sealed_blob->toPlainText().trimmed()) {
      local_sealed_blob->setPlainText(content);
      set_sealed_exchange_state(SealedExchangeState::kGenerated);
    }
  };

  QObject::connect(refresh_local_blob_button, &QPushButton::clicked, [&]() {
    refresh_local_sealed_blob();
    if (transport_combo->currentText() == "sealed-file") {
      append_log(session_log, QString("Refreshed local sealed blob from: %1").arg(local_sealed_blob_path()));
    }
  });

  QObject::connect(apply_remote_blob_button, &QPushButton::clicked, [&]() {
    if (transport_combo->currentText() != "sealed-file") {
      set_sealed_exchange_state(SealedExchangeState::kFailed, "transport is not sealed-file");
      return;
    }

    const QString remote_blob = remote_sealed_blob->toPlainText().trimmed();
    if (remote_blob.isEmpty()) {
      set_sealed_exchange_state(SealedExchangeState::kFailed, "remote sealed blob is empty");
      return;
    }

    const QString path = remote_sealed_blob_path();
    if (path.isEmpty()) {
      set_sealed_exchange_state(SealedExchangeState::kFailed, "signal directory is empty");
      return;
    }

    const QFileInfo info(path);
    QDir().mkpath(info.absolutePath());

    QFile remote_file(path);
    if (!remote_file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
      set_sealed_exchange_state(SealedExchangeState::kFailed, "cannot write remote sealed blob file");
      return;
    }

    remote_file.write(remote_blob.toUtf8());
    remote_file.close();
    set_sealed_exchange_state(SealedExchangeState::kReceived);
    append_log(session_log, QString("Applied remote sealed blob to: %1").arg(path));
  });

  auto* sealed_refresh_timer = new QTimer(session_page);
  sealed_refresh_timer->setInterval(1500);
  QObject::connect(sealed_refresh_timer, &QTimer::timeout, [&]() {
    if (!controller.is_running() || transport_combo->currentText() != "sealed-file") {
      return;
    }
    refresh_local_sealed_blob();
  });
  sealed_refresh_timer->start();

  QDateTime last_preview_modified;
  qint64 last_preview_size = -1;
  QImage last_playback_image;
  QDateTime last_playback_presented_at;
  QSize last_inline_target_size;
  QSize last_popup_target_size;
  bool playback_window_presented = false;
#if defined(_WIN32)
  DirectFramePipeServer direct_frame_pipe_server;
  DirectFramePipeServer navigation_frame_pipe_server;
  bool direct_frame_stream_enabled = false;
  bool navigation_frame_stream_enabled = false;
  std::atomic_bool navigation_frame_update_queued{false};
  QString last_direct_frame_status_;
  quint64 direct_frame_presented_frames = 0;
  quint64 direct_frame_present_failures = 0;
  quint64 direct_frame_present_busy_drops = 0;
  quint64 direct_frame_last_latency_ms = 0;
  QString direct_frame_last_present_error;
  std::atomic_bool direct_frame_update_queued{false};
  std::atomic<quint64> direct_frame_dispatch_notifications{0};
  std::atomic<quint64> direct_frame_dispatch_posts{0};
  std::atomic<quint64> direct_frame_dispatch_coalesced{0};
  std::atomic<qint64> direct_frame_dispatch_queued_at_us{0};
  quint64 direct_frame_dispatch_executed = 0;
  quint64 direct_frame_dispatch_wait_total_us = 0;
  quint64 direct_frame_dispatch_wait_max_us = 0;
  quint64 direct_frame_present_call_total_us = 0;
  quint64 direct_frame_present_call_max_us = 0;
  quint64 direct_frame_handler_total_us = 0;
  quint64 direct_frame_handler_max_us = 0;
  qint64 direct_frame_last_stats_log_ms = QDateTime::currentMSecsSinceEpoch();
  DirectFrameTransportStats direct_frame_last_transport_stats;
  quint64 direct_frame_last_presented_frames = 0;
  quint64 direct_frame_last_present_failures = 0;
  quint64 direct_frame_last_present_busy_drops = 0;
  quint64 direct_frame_last_dispatch_notifications = 0;
  quint64 direct_frame_last_dispatch_posts = 0;
  quint64 direct_frame_last_dispatch_coalesced = 0;
  quint64 direct_frame_last_dispatch_executed = 0;
  quint64 direct_frame_last_dispatch_wait_total_us = 0;
  quint64 direct_frame_last_present_call_total_us = 0;
  quint64 direct_frame_last_handler_total_us = 0;
  auto make_direct_frame_channel_name = []() {
    return QString("redclaw-desktop-frame-%1-%2-%3")
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0'));
  };
  auto reset_direct_frame_stats = [&]() {
    direct_frame_presented_frames = 0;
    direct_frame_present_failures = 0;
    direct_frame_present_busy_drops = 0;
    direct_frame_last_latency_ms = 0;
    direct_frame_last_present_error.clear();
    direct_frame_update_queued.store(false);
    direct_frame_dispatch_notifications.store(0);
    direct_frame_dispatch_posts.store(0);
    direct_frame_dispatch_coalesced.store(0);
    direct_frame_dispatch_queued_at_us.store(0);
    direct_frame_dispatch_executed = 0;
    direct_frame_dispatch_wait_total_us = 0;
    direct_frame_dispatch_wait_max_us = 0;
    direct_frame_present_call_total_us = 0;
    direct_frame_present_call_max_us = 0;
    direct_frame_handler_total_us = 0;
    direct_frame_handler_max_us = 0;
    direct_frame_last_stats_log_ms = QDateTime::currentMSecsSinceEpoch();
    direct_frame_last_transport_stats = DirectFrameTransportStats{};
    direct_frame_last_presented_frames = 0;
    direct_frame_last_present_failures = 0;
    direct_frame_last_present_busy_drops = 0;
    direct_frame_last_dispatch_notifications = 0;
    direct_frame_last_dispatch_posts = 0;
    direct_frame_last_dispatch_coalesced = 0;
    direct_frame_last_dispatch_executed = 0;
    direct_frame_last_dispatch_wait_total_us = 0;
    direct_frame_last_present_call_total_us = 0;
    direct_frame_last_handler_total_us = 0;
    source_activity_revision = 0;
    source_reference_keyframe_id = 0;
    media_budget_waiting = false;
    capture_playback_state = {};
    playback_progress = {};
    retry_capture_button->setVisible(false);
    latest_displayable_frame_id = 0;
    latest_displayable_keyframe_id = 0;
    latest_presented_frame_id = 0;
    receiver_decoded_frames = 0;
    receiver_rendered_frames = 0;
    last_reported_displayable_frame_id = 0;
  };
  auto send_local_keyframe_request = [&](const QString& reason) {
    if (role_combo->currentText() != "controller" || !controller.is_running()) {
      return;
    }
    redclaw::protocol::StreamControlMessageV1 request;
    request.type = redclaw::protocol::StreamControlMessageTypeV1::kKeyframeRequest;
    request.session_epoch = "local";
    request.message_id = ++gui_control_message_id;
    request.sent_at_ms = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    request.payload = reason.left(128).toStdString();
    QString send_error;
    if (!controller.send_control_message(request, &send_error)) {
      append_log(
          session_log,
          QString("GUI keyframe request failed reason=%1 error=%2")
              .arg(reason, send_error));
      return;
    }
    append_log(session_log, QString("GUI keyframe request sent reason=%1").arg(reason));
  };
#endif

  desktop_navigation_panel->set_region_request_callback(
      [&](const std::string& display_id,
          std::uint16_t left,
          std::uint16_t top,
          std::uint16_t right,
          std::uint16_t bottom,
          std::uint64_t revision) {
        if (role_combo->currentText() != "controller" || !controller.is_running()) {
          return false;
        }
        const bool display_changed =
            desktop_navigation_panel->pending_request_changes_display();
        redclaw::protocol::StreamControlMessageV1 request;
        request.type = redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRequest;
        request.session_epoch = "local";
        request.message_id = ++gui_control_message_id;
        request.sent_at_ms = static_cast<std::uint64_t>(
            QDateTime::currentMSecsSinceEpoch());
        request.display_id = display_id;
        request.region_left = left;
        request.region_top = top;
        request.region_right = right;
        request.region_bottom = bottom;
        request.capture_region_revision = revision;
        QString send_error;
        if (!controller.send_control_message(request, &send_error)) {
          append_log(session_log, "Capture region request failed: " + send_error);
          return false;
        }
        if (remote_input_capture->active()) {
          if (display_changed) {
            pending_capture_geometry_revision = 0;
            applied_capture_geometry_revision = 0;
            remote_input_capture->pause(
                true, "Desktop display changed. Click Start Control to resume.");
          } else {
            pending_capture_geometry_revision = revision;
            applied_capture_geometry_revision = 0;
            remote_input_capture->set_local_suspension(
                LocalInputSuspensionReason::kGeometryTransaction, true);
          }
        }
        return true;
      });

  auto set_playback_status_text = [&](const QString& text) {
    playback_status->setText(text);
    playback_window_status->setText(text);
  };

  auto set_playback_placeholder = [&](const QString& text) {
    playback_canvas->clear();
    playback_canvas->setText(text);
  };

  auto position_playback_window = [&]() {
    QScreen* target_screen = window.screen();
    if (target_screen == nullptr) {
      target_screen = app.primaryScreen();
    }

    const QRect available = target_screen != nullptr
        ? target_screen->availableGeometry()
        : window.geometry();
    const int desired_width = available.width() * 3 / 4;
    const int target_width = (std::min)(
        available.width(),
        (std::max)(680, desired_width));
    const int target_height = (std::min)(
        available.height(),
        (std::max)(480, available.height() * 2 / 3));
    playback_window->resize(target_width, target_height);
    playback_window->move(
        available.x() + (available.width() - target_width) / 2,
        available.y() + (available.height() - target_height) / 2);
  };

  auto present_playback_window = [&]() {
    if (role_combo->currentText() != "controller" || playback_window_presented) {
      return;
    }
    playback_window_presented = true;
    position_playback_window();
    playback_window->show();
    playback_window->raise();
    playback_window->activateWindow();
    playback_geometry_controller->publish_initial_viewport();
  };

  auto show_frozen_connection_hint = [&]() {
    if (last_playback_presented_at.isValid()) {
      playback_control_hint->show_hint(QString("Connection interrupted — frozen frame from %1. Reconnecting; input disabled.")
          .arg(last_playback_presented_at.toString("HH:mm:ss")), 0);
    }
  };

  auto reset_playback_surface = [&](const QString& placeholder_text, const QString& status_text,
                                    bool hide_window, bool preserve_frozen_frame = false) {
    const bool keep_agent_workspace_visible = playback_window->isVisible()
        && role_combo->currentText() == "controller";
    remote_input_capture->pause(true, "Playback surface reset.");
    playback_control_hint->hide_hint();
    remote_input_supported = false;
    remote_input_authorized = false;
    remote_input_state = redclaw::protocol::RemoteInputControlStateV1::kUnavailable;
    remote_input_reason = redclaw::protocol::RemoteInputStatusReasonV1::kDisconnected;
    remote_input_frame_ready = false;
    remote_input_request_pending = false;
    pending_capture_geometry_revision = 0;
    applied_capture_geometry_revision = 0;
    last_presented_capture_geometry_revision = 0;
    remote_input_capture->set_remote_frame_size({});
    remote_input_capture->set_desktop_geometry_revision(0);
    refresh_remote_control_ui();
    last_preview_modified = QDateTime();
    last_preview_size = -1;
    if (!preserve_frozen_frame || !last_playback_presented_at.isValid()) {
      last_playback_image = QImage();
      last_playback_presented_at = QDateTime();
      last_inline_target_size = QSize();
      last_popup_target_size = QSize();
      set_playback_placeholder(placeholder_text);
      playback_window_canvas->clear_frame();
    } else {
      show_frozen_connection_hint();
    }
    set_playback_status_text(status_text);
    agent_panel->set_transport_state(false, false, false,
        "Agent channel disconnected. Conversation is preserved while reconnecting.");
    playback_window_presented = keep_agent_workspace_visible;
    if (hide_window && !keep_agent_workspace_visible) {
      playback_window->hide();
    }
  };
  auto make_navigation_frame_channel_name = []() {
    return QString("redclaw-navigation-frame-%1-%2-%3")
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0'));
  };

  auto playback_inline_target_size = [&]() {
    QSize inline_target_size = playback_canvas->contentsRect().size();
    if (inline_target_size.width() < 32 || inline_target_size.height() < 32) {
      inline_target_size = QSize(640, 360);
    }
    return inline_target_size;
  };

  auto playback_popup_target_size = [&]() {
    QSize popup_target_size = playback_canvas_widget->contentsRect().size();
    if (popup_target_size.width() < 64 || popup_target_size.height() < 64) {
      popup_target_size = QSize(1280, 720);
    }
    return popup_target_size;
  };

  auto update_playback_pixmaps = [&](const QImage& image) {
    last_playback_image = image;
    const QSize inline_target_size = playback_inline_target_size();
    last_inline_target_size = inline_target_size;
    playback_canvas->setPixmap(QPixmap::fromImage(image).scaled(
        inline_target_size,
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation));

    const QSize popup_target_size = playback_popup_target_size();
    last_popup_target_size = popup_target_size;
    QString render_error;
    if (playback_window_canvas->present_image(image, &render_error) == PresentOutcome::kFailed) {
      playback_window_status->setText(QString("DirectX playback failed: %1").arg(render_error));
      return false;
    }
    return true;
  };

  auto rescale_cached_playback_image_if_needed = [&]() {
    if (last_playback_image.isNull()) {
      return;
    }
    if (playback_inline_target_size() == last_inline_target_size
        && playback_popup_target_size() == last_popup_target_size) {
      return;
    }
    update_playback_pixmaps(last_playback_image);
  };

  set_playback_placeholder("Your remote desktop will appear here");
  set_playback_status_text("The live desktop will appear here as soon as pairing completes.");

  auto preview_output_path = [&]() {
    const QString dir = signal_dir->text().trimmed().isEmpty()
        ? QString("runtime-signaling")
        : signal_dir->text().trimmed();
    return QDir(dir).filePath("controller-preview.ppm");
  };

  auto refresh_playback_preview = [&]() {
    const QString path = preview_output_path();
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
      set_playback_status_text("Waiting for the first desktop frame.");
      return;
    }

    if (info.lastModified() == last_preview_modified && info.size() == last_preview_size) {
      rescale_cached_playback_image_if_needed();
      return;
    }

    QImage image(path);
    if (image.isNull()) {
      set_playback_status_text("The remote desktop preview is still loading.");
      return;
    }

    last_preview_modified = info.lastModified();
    last_preview_size = info.size();
    if (!update_playback_pixmaps(image)) {
      return;
    }
    set_playback_status_text(QString("Remote desktop live (%1x%2).")
                                 .arg(QString::number(image.width()), QString::number(image.height())));
    remote_input_capture->set_remote_frame_size(image.size());
    remote_input_frame_ready = true;
    last_playback_presented_at = QDateTime::currentDateTime();
    playback_control_hint->hide_hint();
    refresh_remote_control_ui();
    apply_connection_flow_event(ConnectionFlowEvent::kFirstFrameReady);
    present_playback_window();
  };

  // ---------------------------------------------------------------------------
  // Frame-push render handler — called on the main thread via QueuedConnection
  // whenever the shared-memory worker deposits a new frame.
  // ---------------------------------------------------------------------------
  auto handle_direct_frame = [&]() {
    GuiLatencyScope frame_timing(GuiStage::kFrameHandler);
    const auto handler_start = std::chrono::steady_clock::now();
    DirectFrameData direct_frame;
    quint64 direct_frame_count = 0;
    if (!direct_frame_pipe_server.take_latest_frame(&direct_frame, &direct_frame_count)) {
      return;  // spurious wakeup or frame already consumed
    }
    if (!playback_progress.accept(direct_frame.playback_generation, direct_frame.frame_id, direct_frame.keyframe)) {
      direct_frame_pipe_server.recycle_frame(std::move(direct_frame));
      return;
    }
    if (source_reference_keyframe_id != 0
        && latest_displayable_keyframe_id >= source_reference_keyframe_id) {
      (void)send_displayable_stats(true);
    }
    QString render_error;
    const auto present_call_start = std::chrono::steady_clock::now();
    const auto present_outcome =
        playback_window_canvas->present_frame(direct_frame, &render_error);
    const bool present_succeeded = present_outcome == PresentOutcome::kPresented;
    const quint64 present_call_us = static_cast<quint64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - present_call_start)
            .count());
    direct_frame_present_call_total_us += present_call_us;
    direct_frame_present_call_max_us =
        (std::max)(direct_frame_present_call_max_us, present_call_us);
    const bool present_busy_drop = present_outcome == PresentOutcome::kBusyDrop;
    if (present_busy_drop) {
      ++direct_frame_present_busy_drops;
    } else if (present_succeeded) {
      if (playback_progress.mark_presented(direct_frame.frame_id)) {
        ++direct_frame_presented_frames;
        app.latency.record(GuiStage::kDisplayedFrame, 0);
      } else app.latency.record(GuiStage::kRedraw, 0);
      receiver_rendered_frames = direct_frame_presented_frames;
      capture_playback_state.presented(direct_frame.frame_id);
      retry_capture_button->setVisible(capture_playback_state.retry_available());
      direct_frame_last_present_error.clear();
      const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
      if (direct_frame.timestamp_ms > 0 && now_ms >= static_cast<qint64>(direct_frame.timestamp_ms)) {
        direct_frame_last_latency_ms = static_cast<quint64>(now_ms - static_cast<qint64>(direct_frame.timestamp_ms));
      } else {
        direct_frame_last_latency_ms = 0;
      }
      last_inline_target_size = QSize();
      last_popup_target_size = playback_popup_target_size();
      const QSize remote_frame_size(
          static_cast<int>(direct_frame.content_rect_width == 0
              ? direct_frame.width : direct_frame.content_rect_width),
          static_cast<int>(direct_frame.content_rect_height == 0
              ? direct_frame.height : direct_frame.content_rect_height));
      // The playback surface clears its input geometry during reconnect.  Keep
      // the capture owner synchronized on every successfully presented frame;
      // a same-resolution reconnect must restore the geometry as well.
      GuiLatencyScope geometry_timing(GuiStage::kGeometry);
      remote_input_capture->set_remote_frame_size(remote_frame_size);
      last_presented_capture_geometry_revision = direct_frame.capture_region_revision;
      remote_input_capture->set_desktop_geometry_revision(
          direct_frame.capture_region_revision);
      if (pending_capture_geometry_revision != 0
          && applied_capture_geometry_revision >= pending_capture_geometry_revision
          && last_presented_capture_geometry_revision >= pending_capture_geometry_revision) {
        pending_capture_geometry_revision = 0;
        applied_capture_geometry_revision = 0;
        remote_input_capture->set_local_suspension(
            LocalInputSuspensionReason::kGeometryTransaction, false);
      }
      // Only update the status label and banner when something meaningful
      // changes (resolution, backend).  Updating every frame causes Qt to
      // re-layout playback_window_status, which can deliver a resizeEvent to
      // playback_canvas_widget → resize_swap_chain() → DXGI flicker.
      const QString new_status = capture_playback_state.waiting() ? QString::fromUtf8("画面已暂停，等待 Host 恢复采集。") :
          QString("Remote desktop live (%1x%2, backend: %3).")
              .arg(QString::number(direct_frame.width),
                   QString::number(direct_frame.height),
                   playback_window_canvas->backend_name());
      if (new_status != last_direct_frame_status_
          || playback_window_status->text() != new_status) {
        last_direct_frame_status_ = new_status;
        set_playback_status_text(new_status);
        if (debug_status.connected && debug_status.channel_open) {
          set_runtime_banner("Live desktop connected.", "good");
        }
      }
      if (!remote_input_frame_ready && !capture_playback_state.waiting()) {
        remote_input_frame_ready = true;
        playback_control_hint->hide_hint();
        refresh_remote_control_ui();
        present_playback_window();
      }
      if (connection_flow_model.snapshot().status != ConnectionFlowStatus::kComplete) {
        apply_connection_flow_event(ConnectionFlowEvent::kFirstFrameReady);
      }
      last_playback_presented_at = QDateTime::currentDateTime();
      if (debug_status.connected) {
        playback_control_hint->hide_persistent_hint();
      }
    } else {
      ++direct_frame_present_failures;
      app.latency.present_failure(render_error);
      if (direct_frame.output_path == redclaw::render::DecodedVideoFramePath::kD3D11DecodeSurface) {
        direct_frame_pipe_server.request_cpu_decode_fallback();
      }
      direct_frame_last_present_error = render_error;
      const QString fail_status =
          QString("%1 render failed: %2")
              .arg(playback_window_canvas->backend_name(), render_error);
      if (fail_status != last_direct_frame_status_) {
        last_direct_frame_status_ = fail_status;
        set_playback_status_text(fail_status);
        set_runtime_banner(
            QString("%1 render failed.").arg(playback_window_canvas->backend_name()), "bad");
      }
    }
    direct_frame_pipe_server.recycle_frame(std::move(direct_frame));
    const quint64 handler_us = static_cast<quint64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - handler_start)
            .count());
    direct_frame_handler_total_us += handler_us;
    direct_frame_handler_max_us = (std::max)(direct_frame_handler_max_us, handler_us);
  };

  std::function<void()> schedule_direct_frame_update;
  auto* direct_frame_dispatch_target = new LatestFrameDispatchTarget(
      [&]() {
        ++direct_frame_dispatch_executed;
        const qint64 queued_at_us = direct_frame_dispatch_queued_at_us.exchange(0);
        if (queued_at_us > 0) {
          const qint64 dispatch_wait_us = (std::max)(0LL, steady_now_us() - queued_at_us);
          app.latency.record(GuiStage::kDispatchWait, static_cast<std::uint64_t>(dispatch_wait_us));
          direct_frame_dispatch_wait_total_us += static_cast<quint64>(dispatch_wait_us);
          direct_frame_dispatch_wait_max_us = (std::max)(
              direct_frame_dispatch_wait_max_us,
              static_cast<quint64>(dispatch_wait_us));
        }
        direct_frame_update_queued.store(false);
        handle_direct_frame();
        if (direct_frame_pipe_server.has_pending_frame()) {
          schedule_direct_frame_update();
        }
      },
      session_page);
  schedule_direct_frame_update = [&]() {
    ++direct_frame_dispatch_notifications;
    if (direct_frame_update_queued.exchange(true)) {
      ++direct_frame_dispatch_coalesced;
      return;
    }
    direct_frame_dispatch_queued_at_us.store(steady_now_us());
    ++direct_frame_dispatch_posts;
    direct_frame_dispatch_target->post();
  };

#if defined(_WIN32)
  std::function<void()> schedule_navigation_frame_update;
  auto* navigation_frame_dispatch_target = new LatestFrameDispatchTarget(
      [&]() {
        navigation_frame_update_queued.store(false);
        DirectFrameData frame;
        quint64 frame_count = 0;
        if (navigation_frame_pipe_server.take_latest_frame(&frame, &frame_count)) {
          const QImage image(
              frame.pixels.data(),
              static_cast<int>(frame.width),
              static_cast<int>(frame.height),
              static_cast<int>(frame.row_pitch),
              QImage::Format_RGB32);
          desktop_navigation_panel->set_thumbnail(
              frame.display_id,
              image,
              frame.capture_region_revision);
          navigation_frame_pipe_server.recycle_frame(std::move(frame));
        }
        if (navigation_frame_pipe_server.has_pending_frame()) {
          schedule_navigation_frame_update();
        }
      },
      session_page);
  schedule_navigation_frame_update = [&]() {
    if (navigation_frame_update_queued.exchange(true)) {
      return;
    }
    navigation_frame_dispatch_target->post();
  };
#endif

  auto* playback_timer = new QTimer(session_page);
  // Timer is only used for the non-stream preview refresh path.
  // Live desktop frames are pushed immediately via set_on_frame_notify.
  playback_timer->setInterval(100);
  QObject::connect(playback_timer, &QTimer::timeout, [&]() {
    if (direct_frame_stream_enabled) return;
    refresh_playback_preview();
  });
  playback_timer->start();

#if defined(_WIN32)
  auto* displayable_feedback_timer = new QTimer(session_page);
  displayable_feedback_timer->setInterval(250);
  QObject::connect(displayable_feedback_timer, &QTimer::timeout, [&]() {
    if (!direct_frame_stream_enabled || !direct_frame_pipe_server.is_running()) {
      return;
    }
    const auto transport = direct_frame_pipe_server.stats_snapshot();
    playback_progress.observe_transport(transport.playback_generation,
        transport.latest_displayable_frame_id, transport.latest_displayable_keyframe_id);
    receiver_decoded_frames = transport.decoded_frames;
    receiver_rendered_frames = direct_frame_presented_frames;
    (void)send_displayable_stats(false);
  });
  displayable_feedback_timer->start();
#endif

#if defined(_WIN32)
  auto* direct_frame_stats_timer = new QTimer(session_page);
  direct_frame_stats_timer->setInterval(5000);
  QObject::connect(direct_frame_stats_timer, &QTimer::timeout, [&]() {
    if (!direct_frame_stream_enabled || !direct_frame_pipe_server.is_running()) {
      return;
    }

    const auto delta = [](quint64 current, quint64 previous) {
      return current >= previous ? (current - previous) : current;
    };

    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const double window_seconds = (std::max)(0.001, static_cast<double>(now_ms - direct_frame_last_stats_log_ms) / 1000.0);
    const DirectFrameTransportStats transport = direct_frame_pipe_server.stats_snapshot();
    const quint64 writer_delta = delta(transport.writer_frames, direct_frame_last_transport_stats.writer_frames);
    const quint64 skipped_delta = delta(transport.skipped_sequences, direct_frame_last_transport_stats.skipped_sequences);
    const quint64 published_delta = delta(transport.published_frames, direct_frame_last_transport_stats.published_frames);
    const quint64 decoded_delta = delta(transport.decoded_frames, direct_frame_last_transport_stats.decoded_frames);
    const quint64 decode_failures_delta = delta(transport.decode_failures, direct_frame_last_transport_stats.decode_failures);
    const quint64 hardware_decode_fallback_delta = delta(
        transport.hardware_decode_runtime_fallbacks,
        direct_frame_last_transport_stats.hardware_decode_runtime_fallbacks);
    const quint64 hw_transfer_delta = delta(
        transport.hardware_decode_cpu_transfer_frames,
        direct_frame_last_transport_stats.hardware_decode_cpu_transfer_frames);
    const quint64 d3d11_surface_delta = delta(
        transport.d3d11_decode_surface_frames,
        direct_frame_last_transport_stats.d3d11_decode_surface_frames);
    const quint64 d3d11_fallback_delta = delta(
        transport.d3d11_surface_fallbacks,
        direct_frame_last_transport_stats.d3d11_surface_fallbacks);
    const quint64 software_decode_delta = delta(
        transport.software_decode_bgra_frames,
        direct_frame_last_transport_stats.software_decode_bgra_frames);
    const quint64 shared_bgra_delta = delta(
        transport.shared_memory_bgra_frames,
        direct_frame_last_transport_stats.shared_memory_bgra_frames);
    const quint64 buffer_reuse_delta = delta(
        transport.buffer_pool.reuse_hits,
        direct_frame_last_transport_stats.buffer_pool.reuse_hits);
    const quint64 oversize_drops_delta = delta(transport.writer_oversize_drops, direct_frame_last_transport_stats.writer_oversize_drops);
    const quint64 busy_drops_delta = delta(
        transport.writer_busy_drops,
        direct_frame_last_transport_stats.writer_busy_drops);
    const quint64 snapshot_copy_delta = delta(
        transport.shared_snapshot_copies,
        direct_frame_last_transport_stats.shared_snapshot_copies);
    const quint64 snapshot_failure_delta = delta(
        transport.shared_snapshot_failures,
        direct_frame_last_transport_stats.shared_snapshot_failures);
    const quint64 snapshot_sequence_change_delta = delta(
        transport.shared_snapshot_sequence_changes,
        direct_frame_last_transport_stats.shared_snapshot_sequence_changes);
    const quint64 snapshot_total_us_delta = delta(
        transport.shared_snapshot_total_us,
        direct_frame_last_transport_stats.shared_snapshot_total_us);
    const double snapshot_copy_avg_ms = snapshot_copy_delta == 0
        ? 0.0
        : static_cast<double>(snapshot_total_us_delta)
            / 1000.0 / static_cast<double>(snapshot_copy_delta);
    const quint64 presented_delta = delta(direct_frame_presented_frames, direct_frame_last_presented_frames);
    const quint64 present_failures_delta = delta(direct_frame_present_failures, direct_frame_last_present_failures);
    const quint64 present_busy_drops_delta = delta(
        direct_frame_present_busy_drops,
        direct_frame_last_present_busy_drops);
    const quint64 dispatch_notifications = direct_frame_dispatch_notifications.load();
    const quint64 dispatch_posts = direct_frame_dispatch_posts.load();
    const quint64 dispatch_coalesced = direct_frame_dispatch_coalesced.load();
    const quint64 dispatch_notifications_delta = delta(
        dispatch_notifications,
        direct_frame_last_dispatch_notifications);
    const quint64 dispatch_posts_delta = delta(
        dispatch_posts,
        direct_frame_last_dispatch_posts);
    const quint64 dispatch_coalesced_delta = delta(
        dispatch_coalesced,
        direct_frame_last_dispatch_coalesced);
    const quint64 dispatch_executed_delta = delta(
        direct_frame_dispatch_executed,
        direct_frame_last_dispatch_executed);
    const quint64 dispatch_wait_delta_us = delta(
        direct_frame_dispatch_wait_total_us,
        direct_frame_last_dispatch_wait_total_us);
    const quint64 present_call_delta_us = delta(
        direct_frame_present_call_total_us,
        direct_frame_last_present_call_total_us);
    const quint64 handler_delta_us = delta(
        direct_frame_handler_total_us,
        direct_frame_last_handler_total_us);
    const quint64 present_attempt_delta = presented_delta
        + present_failures_delta + present_busy_drops_delta;
    const double average_dispatch_wait_ms = dispatch_executed_delta == 0
        ? 0.0
        : static_cast<double>(dispatch_wait_delta_us)
            / 1000.0 / static_cast<double>(dispatch_executed_delta);
    const double average_present_call_ms = present_attempt_delta == 0
        ? 0.0
        : static_cast<double>(present_call_delta_us)
            / 1000.0 / static_cast<double>(present_attempt_delta);
    const double average_handler_ms = present_attempt_delta == 0
        ? 0.0
        : static_cast<double>(handler_delta_us)
            / 1000.0 / static_cast<double>(present_attempt_delta);
    const quint64 transport_drop_est = writer_delta > published_delta ? (writer_delta - published_delta) : 0;
    const quint64 playback_drop_est = published_delta > presented_delta ? (published_delta - presented_delta) : 0;

    QString health = "healthy";
    if (oversize_drops_delta > 0) {
      health = "writer_oversize_drop";
    } else if (busy_drops_delta > 0) {
      health = "latest_only_busy_drop";
    } else if (present_failures_delta > 0) {
      health = "present_failures";
    } else if (present_busy_drops_delta > 0) {
      health = "present_busy_drop";
    } else if (decode_failures_delta > 0) {
      health = "decode_failures";
    } else if (skipped_delta > 0 || transport_drop_est > 0) {
      health = "transport_drop";
    } else if (playback_drop_est > 0) {
      health = "latest_only_coalescing";
    } else if (writer_delta == 0 && published_delta == 0 && presented_delta == 0) {
      health = "idle";
    }

    QString log_line =
        QString("GUI direct frame stats channel=%1 window_seconds=%2 writer_fps=%3 publish_fps=%4 playback_fps=%5 skipped=%6 writer_oversize_drops=%7 decode_failures=%8 present_failures=%9 transport_drop_est=%10 playback_drop_est=%11 latency_ms=%12 last_sequence=%13 presented_total=%14 health=%15")
            .arg(direct_frame_pipe_server.pipe_name())
            .arg(QString::number(window_seconds, 'f', 1))
            .arg(QString::number(static_cast<double>(writer_delta) / window_seconds, 'f', 1))
            .arg(QString::number(static_cast<double>(published_delta) / window_seconds, 'f', 1))
            .arg(QString::number(static_cast<double>(presented_delta) / window_seconds, 'f', 1))
            .arg(QString::number(skipped_delta))
            .arg(QString::number(oversize_drops_delta))
            .arg(QString::number(decode_failures_delta))
            .arg(QString::number(present_failures_delta))
            .arg(QString::number(transport_drop_est))
            .arg(QString::number(playback_drop_est))
            .arg(QString::number(direct_frame_last_latency_ms))
            .arg(QString::number(transport.last_sequence))
            .arg(QString::number(direct_frame_presented_frames))
            .arg(health);
    const std::string_view output_path =
        redclaw::render::decoded_video_frame_path_name(transport.last_output_path);
    log_line.append(
        QString(" shared_dependency_catchups_total=%1 shared_independent_skips_total=%2 shared_dependency_gaps_total=%3")
            .arg(transport.shared_dependency_catchups)
            .arg(transport.shared_independent_skips)
            .arg(transport.shared_dependency_gaps));
    log_line.append(
        QString(" writer_busy_drops_delta=%1 writer_busy_drops_total=%2 starvation_feedback_source=receiver_reassembly gui_decode_success_delta=%3 gui_decode_success_total=%4 decoder_backend=%5 consecutive_decode_failures=%6 hardware_decode_runtime_fallback_delta=%7 hardware_decode_runtime_fallback_total=%8 decode_output_path=%9 d3d11_surface_delta=%10 d3d11_surface_fallback_delta=%11 hwdecode_cpu_transfer_delta=%12 software_decode_bgra_delta=%13 shared_memory_bgra_delta=%14 buffer_reuse_delta=%15 buffer_pool_retained=%16 buffer_pool_bytes=%17 buffer_pool_discards=%18 buffer_pool_evictions=%19")
            .arg(QString::number(busy_drops_delta))
            .arg(QString::number(transport.writer_busy_drops))
            .arg(QString::number(decoded_delta))
            .arg(QString::number(transport.decoded_frames))
            .arg(transport.decoder_backend)
            .arg(QString::number(transport.consecutive_decode_failures))
            .arg(QString::number(hardware_decode_fallback_delta))
            .arg(QString::number(transport.hardware_decode_runtime_fallbacks))
            .arg(QString::fromLatin1(output_path.data(), static_cast<qsizetype>(output_path.size())))
            .arg(QString::number(d3d11_surface_delta))
            .arg(QString::number(d3d11_fallback_delta))
            .arg(QString::number(hw_transfer_delta))
            .arg(QString::number(software_decode_delta))
            .arg(QString::number(shared_bgra_delta))
            .arg(QString::number(buffer_reuse_delta))
            .arg(QString::number(transport.buffer_pool.retained_buffers))
            .arg(QString::number(transport.buffer_pool.retained_bytes))
            .arg(QString::number(transport.buffer_pool.discarded_releases))
            .arg(QString::number(transport.buffer_pool.evicted_buffers)));
    log_line.append(
        QString(" shared_snapshot_copy_delta=%1 shared_snapshot_copy_total=%2 shared_snapshot_failure_delta=%3 shared_snapshot_failure_total=%4 shared_snapshot_sequence_change_delta=%5 shared_snapshot_sequence_change_total=%6 shared_snapshot_bytes_total=%7 shared_slot_hold_avg_ms=%8 shared_slot_hold_max_ms=%9 dispatch_notify_delta=%10 dispatch_post_delta=%11 dispatch_coalesced_delta=%12 dispatch_executed_delta=%13 dispatch_wait_avg_ms=%14 dispatch_wait_max_ms=%15 present_call_avg_ms=%16 present_call_max_ms=%17 present_busy_drops_delta=%18 present_busy_drops_total=%19 handler_avg_ms=%20 handler_max_ms=%21 dispatch_priority=high")
            .arg(QString::number(snapshot_copy_delta))
            .arg(QString::number(transport.shared_snapshot_copies))
            .arg(QString::number(snapshot_failure_delta))
            .arg(QString::number(transport.shared_snapshot_failures))
            .arg(QString::number(snapshot_sequence_change_delta))
            .arg(QString::number(transport.shared_snapshot_sequence_changes))
            .arg(QString::number(transport.shared_snapshot_bytes))
            .arg(QString::number(snapshot_copy_avg_ms, 'f', 3))
            .arg(QString::number(
                static_cast<double>(transport.shared_snapshot_max_us) / 1000.0,
                'f',
                3))
            .arg(QString::number(dispatch_notifications_delta))
            .arg(QString::number(dispatch_posts_delta))
            .arg(QString::number(dispatch_coalesced_delta))
            .arg(QString::number(dispatch_executed_delta))
            .arg(QString::number(average_dispatch_wait_ms, 'f', 2))
            .arg(QString::number(
                static_cast<double>(direct_frame_dispatch_wait_max_us) / 1000.0,
                'f',
                2))
            .arg(QString::number(average_present_call_ms, 'f', 2))
            .arg(QString::number(
                static_cast<double>(direct_frame_present_call_max_us) / 1000.0,
                'f',
                2))
            .arg(QString::number(present_busy_drops_delta))
            .arg(QString::number(direct_frame_present_busy_drops))
            .arg(QString::number(average_handler_ms, 'f', 2))
            .arg(QString::number(
                static_cast<double>(direct_frame_handler_max_us) / 1000.0,
                'f',
                2)));
    if (!transport.last_decode_error.isEmpty()) {
      QString safe_decode_error = QString::fromStdString(
          redclaw::diag::redact_log_text(transport.last_decode_error.toStdString()));
      safe_decode_error.replace(' ', '_');
      safe_decode_error.replace('\n', '_');
      safe_decode_error.replace('\r', '_');
      log_line.append(QString(" last_decode_error=%1").arg(safe_decode_error));
    }
    if (!transport.last_hardware_decode_error.isEmpty()) {
      QString safe_hardware_error = QString::fromStdString(
          redclaw::diag::redact_log_text(
              transport.last_hardware_decode_error.toStdString()));
      safe_hardware_error.replace(' ', '_');
      safe_hardware_error.replace('\n', '_');
      safe_hardware_error.replace('\r', '_');
      log_line.append(
          QString(" hardware_decode_fallback_from=%1 last_hardware_decode_error=%2")
              .arg(transport.hardware_decode_fallback_from_backend, safe_hardware_error));
    }
    if (!direct_frame_last_present_error.isEmpty()) {
      log_line.append(QString(" last_present_error=%1").arg(direct_frame_last_present_error));
    }
    append_log(session_log, log_line);

    const bool presentation_recovered =
        direct_frame_presented_frames > direct_frame_last_presented_frames;
    const bool phase_changed = presentation_recovered
        && debug_status.phase != "streaming";
    const bool stale_error_cleared = presentation_recovered
        && !debug_status.last_error.empty();
    debug_status.gui_decode_success = transport.decoded_frames;
    debug_status.gui_decode_failures = transport.decode_failures;
    debug_status.gui_presented = direct_frame_presented_frames;
    debug_status.gui_present_failures = direct_frame_present_failures;
    debug_status.direct_pipe_writer_busy_drops = transport.writer_busy_drops;
    debug_status.shared_snapshot_copies = transport.shared_snapshot_copies;
    debug_status.shared_snapshot_failures = transport.shared_snapshot_failures;
    debug_status.shared_snapshot_sequence_changes = transport.shared_snapshot_sequence_changes;
    debug_status.shared_snapshot_max_us = transport.shared_snapshot_max_us;
    debug_status.shared_dependency_catchups = transport.shared_dependency_catchups;
    debug_status.shared_independent_skips = transport.shared_independent_skips;
    debug_status.shared_dependency_gaps = transport.shared_dependency_gaps;
    debug_status.hardware_decode_runtime_fallbacks =
        transport.hardware_decode_runtime_fallbacks;
    debug_status.decoder_backend = transport.decoder_backend.toStdString();
    debug_status.last_decode_error = redclaw::diag::redact_log_text(
        transport.last_decode_error.toStdString());
    debug_status.last_hardware_decode_error = redclaw::diag::redact_log_text(
        transport.last_hardware_decode_error.toStdString());
    if (phase_changed) {
      debug_status.phase = "streaming";
    }
    if (stale_error_cleared) {
      debug_status.last_error.clear();
    }
    persist_debug_status(phase_changed || stale_error_cleared);

    direct_frame_last_stats_log_ms = now_ms;
    direct_frame_last_transport_stats = transport;
    direct_frame_last_presented_frames = direct_frame_presented_frames;
    direct_frame_last_present_failures = direct_frame_present_failures;
    direct_frame_last_present_busy_drops = direct_frame_present_busy_drops;
    direct_frame_last_dispatch_notifications = dispatch_notifications;
    direct_frame_last_dispatch_posts = dispatch_posts;
    direct_frame_last_dispatch_coalesced = dispatch_coalesced;
    direct_frame_last_dispatch_executed = direct_frame_dispatch_executed;
    direct_frame_last_dispatch_wait_total_us = direct_frame_dispatch_wait_total_us;
    direct_frame_last_present_call_total_us = direct_frame_present_call_total_us;
    direct_frame_last_handler_total_us = direct_frame_handler_total_us;
  });
  direct_frame_stats_timer->start();
#endif

  QObject::connect(signal_dir, &QLineEdit::textChanged, [&]() {
    reset_playback_surface(
        "Your remote desktop will appear here",
        "Waiting for the live desktop to appear.",
        true);
  });


  constexpr int kRuntimeOutputUiLogFlushIntervalMs = 50;
  QStringList runtime_output_ui_log_lines;
  auto* runtime_output_ui_log_timer = new QTimer(session_page);
  runtime_output_ui_log_timer->setSingleShot(true);
  runtime_output_ui_log_timer->setTimerType(Qt::PreciseTimer);
  auto flush_runtime_output_ui_log = [&]() {
    if (runtime_output_ui_log_lines.isEmpty()) {
      return;
    }
    const QString batch = runtime_output_ui_log_lines.join('\n');
    runtime_output_ui_log_lines.clear();
    GuiLatencyScope timing(GuiStage::kLogWidget);
    session_log->appendPlainText(batch);
  };
  auto queue_runtime_output_ui_log = [&](const QString& safe_line) {
    const QString formatted = format_gui_log_line(safe_line);
    runtime_output_ui_log_lines.push_back(formatted);
    write_gui_log_sink(formatted);
    if (!runtime_output_ui_log_timer->isActive()) {
      runtime_output_ui_log_timer->start(kRuntimeOutputUiLogFlushIntervalMs);
    }
  };
  QObject::connect(runtime_output_ui_log_timer, &QTimer::timeout, [&]() {
    flush_runtime_output_ui_log();
  });


  auto* agent_drain_timer = new QTimer(session_page);
  std::uint64_t agent_ipc_connection_total = 0;
  std::optional<redclaw::protocol::AgentMessageEnvelopeV1> pending_agent_envelope;
  agent_drain_timer->setInterval(50);
  QObject::connect(agent_drain_timer, &QTimer::timeout, session_page, [&]() {
    GuiLatencyScope receive_timing(GuiStage::kAgentReceive);
    const auto ipc = controller.agent_pipe_stats();
    agent_panel->set_transport_state(debug_status.agent_authorized,
        debug_status.agent_channel_open && ipc.connected && agent_control_server->available(),
        agent_control_server->sync_required());
    if (ipc.connection_total > agent_ipc_connection_total) {
      if (agent_ipc_connection_total != 0) {
        agent_control_server->request_sync();
      }
      agent_ipc_connection_total = ipc.connection_total;
    }
    QElapsedTimer receive_budget;
    receive_budget.start();
    for (int count = 0; count < 64 && receive_budget.nsecsElapsed() < 1000000; ++count) {
      if (!pending_agent_envelope) {
        auto incoming = controller.take_agent_messages();
        if (incoming.empty()) break;
        pending_agent_envelope = std::move(incoming.front());
      }
      if (pending_agent_envelope->type == redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest) {
        // Runtime-local delivery loss asks the coordinator for its durable ACK,
        // never a zero-watermark replay or a desktop connection rebuild.
        agent_control_server->request_sync(pending_agent_envelope->task_id);
        pending_agent_envelope.reset();
        continue;
      }
      if (!agent_control_server->observe_remote(*pending_agent_envelope)) break;
      pending_agent_envelope.reset();
    }
    QElapsedTimer budget;
    receive_timing.finish();
    GuiLatencyScope apply_timing(GuiStage::kAgentApply);
    budget.start();
    while (budget.nsecsElapsed() < 2000000) {
      auto messages = agent_control_server->take_durable_messages();
      if (messages.empty()) break;
      const auto& message = messages.front();
      if (message.type == redclaw::protocol::AgentMessageTypeV1::kCapabilities) {
        agent_panel->apply_capability_message(message);
      } else if (message.type == redclaw::protocol::AgentMessageTypeV1::kProjectCatalog) {
        agent_panel->apply_project_catalog_message(message);
      } else {
        agent_panel->apply_task_message(message);
      }
    }
  });
  agent_drain_timer->start();
  auto handle_runtime_control = [&](const redclaw::protocol::StreamControlMessageV1& control,
                                    std::uint64_t emitted_monotonic_us) {
          if (control.type
              == redclaw::protocol::StreamControlMessageTypeV1::kSourceActivityState) {
            if (playback_progress.observe_source(control.session_epoch)) {
#if defined(_WIN32)
              playback_progress.reset_frames(direct_frame_pipe_server.request_session_reset());
#endif
              capture_playback_state = {};
              remote_input_frame_ready = false;
              remote_input_capture->pause(true, "Host session changed; waiting for a new frame.");
            }
            const bool was_waiting = capture_playback_state.waiting();
            capture_playback_state.observe(control);
            retry_capture_button->setVisible(capture_playback_state.retry_available());
            if (capture_playback_state.waiting()) {
              remote_input_capture->pause(true, "Host capture unavailable.");
              remote_input_frame_ready = false;
              remote_input_request_pending = false;
              set_playback_status_text(QString::fromUtf8("画面已暂停。恢复后请重新点击 Start Control 开启控制。"));
              // Capture may pause before the first frame opens the remote window.
              // Present the recovery controls without marking a frame as ready.
              present_playback_window();
            } else if (was_waiting) {
              remote_input_frame_ready = true;
              set_playback_status_text(QString::fromUtf8("画面已恢复。请点击 Start Control 开启控制。"));
            }
            refresh_remote_control_ui();
            source_activity_revision = control.source_activity_revision;
            source_reference_keyframe_id = control.reference_keyframe_id;
            media_budget_waiting = control.media_budget_waiting;
            if (media_budget_waiting && !capture_playback_state.waiting()) {
              set_playback_status_text("Bandwidth insufficient for a clear frame. Retrying; the last clear frame is retained.");
            }
            last_reported_displayable_frame_id = 0;
            const bool crossed_reference = source_reference_keyframe_id != 0
                && latest_displayable_keyframe_id >= source_reference_keyframe_id;
            (void)send_displayable_stats(crossed_reference);
            return;
          }
          if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kWorkspace) {
            file_transfer_panel->receive(control); return;
          }
          if (control.type
              == redclaw::protocol::StreamControlMessageTypeV1::kDesktopDisplayCatalog) {
            desktop_navigation_panel->set_display_catalog(
                control.desktop_displays, control.display_catalog_revision);
            desktop_navigation_panel->set_transport_available(true);
            return;
          }
          if (control.type
              == redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionApplied) {
            desktop_navigation_panel->apply_region_applied(control);
#if defined(_WIN32) && !defined(NDEBUG)
            if (qa_input_probe) qa_input_probe->set_region(control);
#endif
            remote_input_capture->set_desktop_geometry_revision(
                control.capture_region_revision);
            if (pending_capture_geometry_revision == control.capture_region_revision) {
              applied_capture_geometry_revision = control.capture_region_revision;
              if (last_presented_capture_geometry_revision
                  >= pending_capture_geometry_revision) {
                pending_capture_geometry_revision = 0;
                applied_capture_geometry_revision = 0;
                remote_input_capture->set_local_suspension(
                    LocalInputSuspensionReason::kGeometryTransaction, false);
              }
            }
            remote_input_request_pending = false;
            refresh_remote_control_ui();
            return;
          }
          if (control.type
              == redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRejected) {
            desktop_navigation_panel->apply_region_rejected(control);
            if (pending_capture_geometry_revision == control.capture_region_revision) {
              pending_capture_geometry_revision = 0;
              applied_capture_geometry_revision = 0;
              remote_input_capture->set_local_suspension(
                  LocalInputSuspensionReason::kGeometryTransaction, false);
            }
            return;
          }
          if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kInputCapabilities) {
#if defined(_WIN32) && !defined(NDEBUG)
            if (qa_input_probe) qa_input_probe->set_geometry(control);
#endif
            remote_input_supported = control.input_supported;
            remote_input_authorized = control.input_authorized;
            remote_input_state = control.input_state;
            remote_input_reason = control.input_reason;
            remote_input_capture->set_desktop_geometry_revision(
                control.desktop_geometry_revision);
            if (!remote_input_authorized && remote_input_capture->active()) {
              remote_input_capture->pause(false, "Host authorization was revoked.");
            }
            append_log(
                session_log,
                QString("Remote input capabilities supported=%1 authorized=%2 geometry_revision=%3")
                    .arg(remote_input_supported ? "true" : "false")
                    .arg(remote_input_authorized ? "true" : "false")
                    .arg(QString::number(control.desktop_geometry_revision)));
            refresh_remote_control_ui();
            return;
          }
          if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kInputControlStatus) {
            const auto consumed_us = gui_monotonic_us();
            const bool status_changed = control.input_state != remote_input_state
                || control.input_reason != remote_input_reason
                || control.input_supported != remote_input_supported
                || control.input_authorized != remote_input_authorized;
            remote_input_supported = control.input_supported;
            remote_input_authorized = control.input_authorized;
            remote_input_state = control.input_state;
            remote_input_reason = control.input_reason;
            remote_input_request_pending = false;
            const auto ack_now_ms = static_cast<std::uint64_t>(
                QDateTime::currentMSecsSinceEpoch());
            const bool input_ack = remote_input_capture->acknowledge_input_sequence(
                control.input_sequence, consumed_us, emitted_monotonic_us);
            if (!input_ack && emitted_monotonic_us && consumed_us >= emitted_monotonic_us)
              app.latency.record(GuiStage::kControlStatusLocalDelivery, consumed_us - emitted_monotonic_us,
                  0, emitted_monotonic_us, consumed_us);
            const auto input_state_name = redclaw::protocol::to_string(control.input_state);
            const auto input_reason_name = redclaw::protocol::to_string(control.input_reason);
            const QString input_state_text = QString::fromLatin1(
                input_state_name.data(), static_cast<qsizetype>(input_state_name.size()));
            const QString input_reason_text = QString::fromLatin1(
                input_reason_name.data(), static_cast<qsizetype>(input_reason_name.size()));
            if (control.input_state == redclaw::protocol::RemoteInputControlStateV1::kActive
                && !capture_playback_state.control_allowed()) {
              remote_input_capture->pause(false, "Capture recovery requires a new user control request.");
              (void)send_input_control_request(false);
            } else if (control.input_state == redclaw::protocol::RemoteInputControlStateV1::kActive) {
              QString activation_error;
              if (!remote_input_capture->activate(&activation_error)) {
                append_log(
                    session_log,
                    QString("Remote input capture could not start: %1").arg(activation_error));
                (void)send_input_control_request(false);
              } else {
                playback_control_hint->hide_hint();
              }
            } else if (remote_input_capture->active()) {
              remote_input_capture->pause(
                  false,
                  QString("Host input state changed: %1").arg(input_reason_text));
            }
            if (status_changed
                || static_cast<qint64>(ack_now_ms) - last_remote_input_metrics_log_ms >= 1000) {
              append_log(
                  session_log,
                  QString("Remote input status state=%1 reason=%2 authorized=%3 last_applied_sequence=%4 ack_rtt_ms=%5 ack_p95_ms=%6 sent_batches=%7 merged_moves=%8 mouse_loopback_suppressed=%9")
                      .arg(input_state_text)
                      .arg(input_reason_text)
                      .arg(remote_input_authorized ? "true" : "false")
                      .arg(QString::number(control.input_sequence))
                      .arg(QString::number(remote_input_capture->last_application_ack_rtt_ms()))
                      .arg(QString::number(remote_input_capture->application_ack_p95_ms()))
                      .arg(QString::number(remote_input_capture->sent_batch_count()))
                      .arg(QString::number(remote_input_capture->merged_mouse_move_count()))
                      .arg(QString::number(remote_input_capture->suppressed_mouse_loopback_count())));
              last_remote_input_metrics_log_ms = static_cast<qint64>(ack_now_ms);
            }
            refresh_remote_control_ui();
            return;
          }
          if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogChunk) {
            const QString peer_request_id = QString::fromStdString(control.request_id);
            if (control.gap) {
              remote_log->appendPlainText("--- remote log gap: older lines were evicted ---");
            }
            remote_log_cursor = (std::max)(remote_log_cursor, control.log_cursor);
            const std::string safe_payload = redclaw::diag::redact_log_text(control.payload);
            const QString safe_payload_text = QString::fromUtf8(
                safe_payload.data(), static_cast<qsizetype>(safe_payload.size()));
            remote_log->appendPlainText(safe_payload_text);
            debug_remote_log_snapshot.append_chunk(peer_request_id, safe_payload_text, control.gap);
            runtime_detail_tabs->setVisible(true);
            return;
          }
          if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogComplete) {
            remote_log_cursor = (std::max)(remote_log_cursor, control.log_cursor);
            remote_log->appendPlainText("--- remote log snapshot complete ---");
            debug_remote_log_snapshot.complete(QString::fromStdString(control.request_id));
            return;
          }
          if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kRemoteLogError) {
            const QString safe_error = QString::fromStdString(
                redclaw::diag::redact_log_text(control.payload));
            remote_log->appendPlainText(QString("--- remote log error: %1 ---").arg(safe_error));
            debug_remote_log_snapshot.fail(QString::fromStdString(control.request_id), safe_error);
            runtime_detail_tabs->setVisible(true);
            return;
          }
          if (control.type == redclaw::protocol::StreamControlMessageTypeV1::kStreamTargetApplied) {
            const QString applied = QString(
                "Remote encoder target applied transaction_id=%1 encoded_width=%2 encoded_height=%3 fps=%4 bitrate_kbps=%5")
                .arg(QString::number(control.log_cursor))
                .arg(QString::number(control.encoded_width))
                .arg(QString::number(control.encoded_height))
                .arg(QString::number(control.target_fps))
                .arg(QString::number(control.target_bitrate_kbps));
            append_log(session_log, applied);
            playback_window_status->setText(applied);
            return;
          }
  };
  auto handle_runtime_diagnostic = [&](const QString& trimmed) {
        const QString safe_trimmed = QString::fromStdString(
            redclaw::diag::redact_log_text(trimmed.toStdString()));
        queue_runtime_output_ui_log(safe_trimmed);

        const std::string previous_phase = debug_status.phase;
        const bool debug_status_changed = redclaw::diag::update_debug_runtime_status_from_line(
            trimmed.toStdString(),
            &debug_status);
        if (debug_status_changed) {
          agent_panel->set_transport_state(
              debug_status.agent_authorized,
              debug_status.agent_channel_open && controller.agent_pipe_stats().connected
                  && agent_control_server->available(),
              agent_control_server != nullptr && agent_control_server->sync_required());
          persist_debug_status(previous_phase != debug_status.phase);
        }

        if (trimmed == "Runtime connection_auth accepted") {
          connection_entry_page->password_panel()->accepted();
          set_link_status("Connection password verified.", "good");
        } else if (trimmed == "Runtime connection_auth verifying") {
          set_link_status("Verifying connection password...", "warn");
        } else if (trimmed.startsWith("Runtime connection_auth rejected reason=")) {
          connection_auth_failure = trimmed.contains("protocol_version_incompatible")
              ? "The other device does not support connection passwords. Upgrade it before connecting."
              : trimmed.contains("cooldown") ? "Too many incorrect passwords. Retry after 30 seconds."
              : trimmed.contains("timeout") ? "Connection password verification timed out. Retry the connection."
              : "Connection password rejected. Enter the Host's password and reconnect.";
          if (role_combo->currentText() == "controller") connection_entry_page->password_panel()->rejected();
          set_link_status(connection_auth_failure, "bad");
        }
        const ConnectionFlowRole flow_role = role_combo->currentText() == "host"
            ? ConnectionFlowRole::kHost
            : ConnectionFlowRole::kController;
        const ConnectionFlowLineEvent flow_event = classify_connection_flow_line(trimmed, flow_role);
        if (flow_event.matched) {
          QString flow_detail = flow_event.detail;
          if (flow_event.event == ConnectionFlowEvent::kFatalFailure) {
            flow_detail = trimmed.contains("Runtime signaling timeout")
                ? "No waiting device was found for that code. Confirm the other device is waiting, then retry."
                : "The secure connection could not be established. Check the other device and network path, then retry.";
          }
          apply_connection_flow_event(flow_event.event, flow_detail);
        }
        if (debug_status_changed) {
          const auto previous_flow_status = connection_flow_model.snapshot().status;
          const bool previous_transport_ready = connection_flow_model.snapshot().transport_ready;
          connection_flow_model.observe_transport(debug_status.connected, debug_status.channel_open);
          if (previous_flow_status != connection_flow_model.snapshot().status
              || previous_transport_ready != connection_flow_model.snapshot().transport_ready) {
            refresh_connection_flow_page();
          }
        }

        if (trimmed.contains("Runtime process started:")) {
          set_process_state("running");
          set_runtime_banner("Waiting for the other device to finish pairing.", "warn");
          append_runtime_event(redclaw::render::RuntimeStatusSeverity::kInfo, "process", trimmed.toStdString());
          if (transport_combo->currentText() == "sealed-file") {
            set_sealed_exchange_state(SealedExchangeState::kWaitingRemote);
          }
        }

        if (trimmed.contains("Runtime process finished:")) {
          set_process_state("stopped");
          append_runtime_event(redclaw::render::RuntimeStatusSeverity::kInfo, "process", trimmed.toStdString());
        }

        if (trimmed.startsWith("Runtime state role=")) {
          const QRegularExpression re("state=(\\d+)\\s+remote_description_applied=(\\w+)\\s+connected=(\\w+)");
          const QRegularExpressionMatch match = re.match(trimmed);
          if (match.hasMatch()) {
            const QString ice_state = map_ice_state(match.capturedView(1));
            const bool remote_description_applied = match.captured(2).compare("true", Qt::CaseInsensitive) == 0;
            const bool connected = match.captured(3).compare("true", Qt::CaseInsensitive) == 0;
            set_connection_state_display(ice_state);
            set_handshake_state(remote_description_applied);
            set_live_link_state(connected);
            if (!connected) {
              show_frozen_connection_hint();
            }
            if (connected) {
              if (capture_playback_state.waiting()) {
                set_playback_status_text(QString::fromUtf8("画面已暂停，等待 Host 恢复采集。"));
              } else if (media_budget_waiting) {
                set_playback_status_text("Bandwidth insufficient for a clear frame. Retrying; the last clear frame is retained.");
              } else if (!remote_input_frame_ready) {
                set_playback_status_text("Connection ready. Waiting for the first desktop frame.");
              }
              set_runtime_banner("Connection established.", "good");
              set_link_workflow_running(true, "Session Connected");
              set_link_status(remote_input_frame_ready ? "Connected. Remote desktop live."
                  : "Connected. Waiting for the first remote desktop frame.", "good");
            } else if (remote_description_applied) {
              remote_input_capture->pause(true, "Connection is not active.");
              set_runtime_banner("Secure pairing completed. Finalizing the live connection.", "info");
              set_link_status("Connecting: secure offer/answer applied; establishing the ICE path.", "warn");
            } else {
              remote_input_capture->pause(true, "Connection is not active.");
              set_runtime_banner("Exchanging secure pairing details.", "warn");
              set_link_status("Connecting: waiting for secure offer/answer exchange.", "warn");
            }
            append_runtime_event(redclaw::render::RuntimeStatusSeverity::kInfo,
                                 "state",
                                 QString("state=%1 remote_desc=%2 connected=%3")
                                     .arg(connection_state_value->text(), remote_desc_value->text(), connected_value->text())
                                     .toStdString());
          }
        }

        if (trimmed.startsWith("Runtime candidate stats role=")) {
          candidate_stats_value->setText(safe_trimmed);
          append_runtime_event(redclaw::render::RuntimeStatusSeverity::kInfo, "candidate-stats", trimmed.toStdString());
        }

        if (trimmed.startsWith("Runtime ICE config ")) {
          ice_config_value->setText(safe_trimmed);
          append_runtime_event(redclaw::render::RuntimeStatusSeverity::kInfo, "ice-config", trimmed.toStdString());
        }

        if (trimmed.startsWith("Runtime ICE port mapping role=")) {
          nat_diagnostics_value->setText(safe_trimmed);
          append_runtime_event(
              redclaw::render::RuntimeStatusSeverity::kInfo,
              "nat-diagnostics",
              trimmed.toStdString());
        }

        if (trimmed.startsWith("Runtime rendezvous stats role=")) {
          rendezvous_stats_value->setText(safe_trimmed);
          append_runtime_event(redclaw::render::RuntimeStatusSeverity::kInfo, "rendezvous-stats", trimmed.toStdString());
        }

        if (trimmed.startsWith("Runtime DHT config role=")
            || trimmed.startsWith("Runtime DHT helper detail role=")
            || trimmed.startsWith("Runtime DHT stats role=")) {
          dht_stats_value->setText(safe_trimmed);
          append_runtime_event(redclaw::render::RuntimeStatusSeverity::kInfo, "dht-stats", trimmed.toStdString());
        }

        if (trimmed.startsWith("Runtime NAT diagnostics role=")) {
          nat_diagnostics_value->setText(safe_trimmed);
          append_runtime_event(redclaw::render::RuntimeStatusSeverity::kInfo, "nat-diagnostics", trimmed.toStdString());
          if (trimmed.contains("failure_class=relay_required")) {
            last_failure_value->setText(safe_trimmed);
          }
        }

        if (trimmed.startsWith("Runtime tcp stats role=")) {
          tcp_stats_value->setText(safe_trimmed);
          append_runtime_event(redclaw::render::RuntimeStatusSeverity::kInfo, "tcp-stats", trimmed.toStdString());
        }

        if (trimmed.startsWith("Runtime desktop stream stats role=")) {
          stream_stats_value->setText(safe_trimmed);
          append_runtime_event(redclaw::render::RuntimeStatusSeverity::kInfo, "stream-stats", trimmed.toStdString());
          if (trimmed.contains("captured=0") && trimmed.contains("synthetic=")) {
            last_failure_value->setText(safe_trimmed);
          }
          if (role_combo->currentText() == "host") {
            const auto previous_flow_status = connection_flow_model.snapshot().status;
            connection_flow_model.observe_host_frames(
                debug_status.captured, debug_status.encoded, debug_status.transmitted);
            if (previous_flow_status != connection_flow_model.snapshot().status) {
              refresh_connection_flow_page();
            }
          }
        }

        if (trimmed.startsWith("Runtime desktop stream preview received role=")) {
          set_playback_status_text("Diagnostic preview received. Waiting for the encoded desktop stream.");
        }

        if (trimmed.startsWith("Runtime desktop stream video frame received role=")) {
          set_playback_status_text("Remote desktop frame received. Preparing the live view.");
#if defined(_WIN32)
          if (!direct_frame_stream_enabled) {
            refresh_playback_preview();
          }
#else
          refresh_playback_preview();
#endif
        }

        if (trimmed.startsWith("Runtime tcp failure detail role=")) {
          last_failure_value->setText(safe_trimmed);
          append_runtime_event(redclaw::render::RuntimeStatusSeverity::kWarning, "tcp-failure", trimmed.toStdString());
        }

        if (trimmed.contains("Runtime signaling timeout")) {
          set_runtime_banner("No waiting device was found for that code.", "bad");
          set_link_status("No waiting device was found. On the other device, press Wait with This Code, then try Connect again.", "bad");
          last_failure_value->setText(safe_trimmed);
          append_runtime_event(redclaw::render::RuntimeStatusSeverity::kError, "runtime", trimmed.toStdString());
        }

        if (trimmed.contains("Runtime ICE state failed before DHT signaling completed; continuing")
            || trimmed.contains("Runtime ICE state failed; DHT retry remains active")) {
          set_runtime_banner("This connection attempt failed. Retrying automatically.", "warn");
          set_link_workflow_running(true, "Session in Progress — Retrying");
          set_link_status("The current ICE attempt failed; RedClaw is still running and will retry automatically. Do not press Connect again.", "warn");
          last_failure_value->setText(safe_trimmed);
          append_runtime_event(redclaw::render::RuntimeStatusSeverity::kWarning, "runtime", trimmed.toStdString());
        } else if (trimmed.contains("Runtime signaling failed") || trimmed.contains("Runtime ICE state failed")) {
          set_runtime_banner("Pairing failed. Check the Host and network path, then try again.", "bad");
          set_link_status("Pairing stopped. Check that the Host is waiting and TURN is available when required, then press Connect again.", "bad");
          last_failure_value->setText(safe_trimmed);
          append_runtime_event(redclaw::render::RuntimeStatusSeverity::kError, "runtime", trimmed.toStdString());
          if (transport_combo->currentText() == "sealed-file") {
            set_sealed_exchange_state(SealedExchangeState::kFailed, "runtime signaling failed");
          }
        }

        if (trimmed.startsWith("Runtime rebuilding signaling session role=")) {
          const QRegularExpression retry_re("attempt=(\\d+)");
          const QRegularExpressionMatch retry_match = retry_re.match(trimmed);
          const QString attempt = retry_match.hasMatch() ? retry_match.captured(1) : "next";
          set_connection_state_display("connecting");
          set_handshake_state(false);
          set_live_link_state(false);
          set_link_workflow_running(true, "Session in Progress — Retrying");
          set_runtime_banner(QString("Retrying the connection automatically (attempt %1).").arg(attempt), "warn");
          set_link_status(
              QString("Retrying automatically (attempt %1). The same code and network settings remain active; no action is required.").arg(attempt),
              "warn");
        }

        if (trimmed.startsWith("Runtime remote description applied role=")) {
          set_handshake_state(true);
          set_link_status(
              role_combo->currentText() == "controller"
                  ? "Connecting: Host offer received; publishing this device's answer."
                  : "Connecting: Controller answer received; establishing the ICE path.",
              "warn");
        }

        if (trimmed.startsWith("Runtime DHT signal published role=controller")) {
          if (trimmed.contains("phase=description-only")) {
            set_link_status("Connecting: answer published; sending the best network candidate next.", "warn");
          } else if (trimmed.contains("phase=priority-candidates")
                     || trimmed.contains("phase=direct-candidate")) {
            set_link_status("Connecting: answer and priority network candidate published; waiting for ICE connectivity.", "warn");
          } else if (trimmed.contains("phase=full")) {
            set_link_status("Connecting: full network candidate set published; waiting for ICE connectivity.", "warn");
          }
        }


        if (trimmed.startsWith("Runtime rebuilding signaling session")) {
          remote_input_capture->pause(true, "Connection is reconnecting.");
          remote_input_request_pending = false;
          refresh_remote_control_ui();
#if defined(_WIN32)
          playback_progress = {};
          playback_progress.reset_frames(direct_frame_pipe_server.request_session_reset());
          capture_playback_state = {};
          source_activity_revision = source_reference_keyframe_id = 0;
#endif
          reset_playback_surface(
              "Waiting for the reconnected desktop stream",
              "Connection interrupted. Reconnecting automatically...",
              false, true);
        }

        if (trimmed.contains("applied from sealed blob") || trimmed.contains("remote description applied role=") && trimmed.contains("(sealed-file)")) {
          set_sealed_exchange_state(SealedExchangeState::kApplied);
        }
  };
  auto* runtime_stdio_reader = new RuntimeStdioReader(controller.process(), handle_runtime_control,
      handle_runtime_diagnostic, [&](const QString& error, bool control_failed) {
        if (control_failed) {
          remote_input_capture->pause(true, error);
          remote_input_authorized = false;
          refresh_remote_control_ui();
        } else diagnostic_writer.record_external_loss(error);
        status_label->setText(error);
      }, session_page);

  QObject::connect(
      &controller.process(),
      QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
      [&](int exit_code, QProcess::ExitStatus exit_status) {
        file_transfer_panel->runtime_stopped();
        const bool debug_reconnect = debug_reconnect_requested;
        debug_reconnect_requested = false;
        const bool host_persist_wait = integration_persist_host_wait
            && role_combo->currentText() == "host"
            && !manual_runtime_stop_requested
            && !debug_reconnect;
        const bool manual_stop = manual_runtime_stop_requested;
        manual_runtime_stop_requested = false;
        const bool expected_stop = manual_stop || debug_reconnect;
        const bool clean_exit = expected_stop
            || (exit_status == QProcess::NormalExit && exit_code == 0);

#if defined(_WIN32)
        direct_frame_pipe_server.stop();
        navigation_frame_pipe_server.stop();
        direct_frame_stream_enabled = false;
        navigation_frame_stream_enabled = false;
        desktop_navigation_panel->set_transport_available(false);
#endif
        const QString status = expected_stop
            ? "Stopped by request"
            : (exit_status == QProcess::NormalExit)
            ? QString("Stopped (exit=%1)").arg(exit_code)
            : QString("Crashed (exit=%1)").arg(exit_code);
        status_label->setText(status);
        set_process_state(clean_exit ? "stopped" : "failed");
        debug_status.runtime_running = false;
        debug_status.runtime_pid = 0;
        debug_status.connected = false;
        debug_status.channel_open = false;
        debug_status.ice_state = "closed";
        debug_status.phase = clean_exit ? "stopped" : "failed";
        if (!clean_exit) {
          debug_status.last_error = status.toStdString();
        } else if (expected_stop) {
          debug_status.last_error.clear();
        }
        persist_debug_status(true);
        set_connection_state_display("closed");
        set_handshake_state(false);
        set_live_link_state(false);
        stop_button->setEnabled(false);
        start_button->setEnabled(true);
        set_link_workflow_running(false);
        append_log(session_log, QString("Runtime process finished: %1").arg(status));
        append_runtime_event(
          clean_exit
            ? redclaw::render::RuntimeStatusSeverity::kInfo
            : redclaw::render::RuntimeStatusSeverity::kError,
          "process",
          status.toStdString());

        if (exit_code == 78 && !expected_stop) {
          reset_playback_surface("Your remote desktop will appear here", "Connection authentication failed.", true);
          connection_flow_model.reset(); refresh_connection_flow_page();
          set_link_status(connection_auth_failure.isEmpty()
              ? "Connection credentials are unavailable. Set or enter the connection password."
              : connection_auth_failure, "bad");
          return;
        }
        if (debug_reconnect || host_persist_wait) {
          // Explicit reconnect may start a new flow after Stop completed;
          // late runtime retry notifications must never undo a pending Stop.
          apply_connection_flow_event(debug_reconnect
              ? ConnectionFlowEvent::kStarted : ConnectionFlowEvent::kAutomaticRetry);
        } else if (manual_stop) {
          apply_connection_flow_event(ConnectionFlowEvent::kExitCompleted);
        } else {
          apply_connection_flow_event(
              ConnectionFlowEvent::kFatalFailure,
              clean_exit
                  ? "The connection ended. Retry with the same code or exit."
                  : "The connection process ended unexpectedly. Retry or exit.");
        }

        if (!clean_exit) {
          set_runtime_banner("The session ended unexpectedly.", "bad");
          set_link_status("The session ended unexpectedly. Check the log, then retry with the same code when the Host is ready.", "bad");
          last_failure_value->setText(status);
          reset_playback_surface(
              "Your remote desktop will appear here",
              "The session ended unexpectedly.",
              true);
        } else {
          set_runtime_banner("Connection closed. You can start another session.", "info");
          set_link_status("Connection closed. You can start another session.", "info");
          reset_playback_surface(
              "Your remote desktop will appear here",
              "Connection closed. Start another session when ready.",
              true);
        }

        if (debug_reconnect) {
          set_runtime_banner("Restarting the current debug session.", "info");
          set_link_status("Restarting with the same validated configuration.", "info");
          append_log(session_log, "Debug control: scheduling runtime reconnect with the current configuration.");
          QTimer::singleShot(500, &window, [start_button]() {
            start_button->click();
          });
        } else if (host_persist_wait) {
          set_runtime_banner("Session ended. Restarting host wait with the same code.", "info");
          set_link_status("Session ended. Waiting again for the next connection.", "info");
          append_log(session_log, "Host persist wait: scheduling automatic re-wait with the current code.");
          QTimer::singleShot(1500, &window, [start_button]() {
            start_button->click();
          });
        } else if (manual_stop) {
          set_link_status("Connection stopped. Start another connection when ready.", "info");
        }
      });

  auto reject_session_start = [&](const QString& status_text, const QString& log_text, const QString& visible_text) {
    set_link_workflow_running(false);
#if defined(_WIN32)
    direct_frame_pipe_server.stop();
    navigation_frame_pipe_server.stop();
    direct_frame_stream_enabled = false;
    navigation_frame_stream_enabled = false;
    desktop_navigation_panel->set_transport_available(false);
#endif
    status_label->setText(status_text);
    append_log(session_log, log_text);
    set_process_state("failed");
    set_runtime_banner(visible_text, "bad");
    set_link_status(visible_text, "bad");
    if (connection_flow_model.snapshot().view == ConnectionFlowView::kPreConnection) {
      tabs->setCurrentWidget(session_scroll);
    } else {
      apply_connection_flow_event(ConnectionFlowEvent::kFatalFailure, visible_text);
    }
  };

  QObject::connect(start_button, &QPushButton::clicked, [&]() {
    sync_link_network_settings_to_session_form();

    QString role = role_combo->currentText();
    if (!connection_credential_error.isEmpty()) { set_link_status(connection_credential_error, "bad"); return; }
    redclaw::security::ConnectionCredential connection_credential;
    QString password_error;
    if (!connection_entry_page->password_panel()->prepare(role == "host", &connection_credential, &password_error)) {
      set_link_status(password_error, "bad"); return;
    }
    connection_auth_failure.clear();

    if (role == "host") {
      host_wait_remote_input_authorized =
          connection_entry_page->allow_remote_control_checkbox()->isChecked();
    }
    host_wait_remote_agent_authorized = (role == "host"
        ? connection_entry_page->allow_remote_agent_checkbox()
        : connection_entry_page->allow_controller_agent_checkbox())->isChecked();
    if (host_wait_remote_agent_authorized
        && ((!gui_auto_start.agent_qa_fixture_provider && !agent_settings_dialog->any_provider_ready())
            || agent_settings_dialog->project_roots().isEmpty())) {
      set_link_status("Local Agent execution needs a ready provider and registered project.", "bad");
      return;
    }
    QString transport = transport_combo->currentText();
    QString host = target_host->text().trimmed();
    const QString rendezvous = rendezvous_url->text().trimmed();
    const QString code = session_code->text().trimmed().toUpper();
    const QString passphrase = signal_passphrase->text().trimmed();
    const QString profile_path = runtime_config_path->text().trimmed();
    const bool use_form_override = runtime_form_override->isChecked() || profile_path.isEmpty();
    const QString network_bind_address = link_network_exit->currentData(Qt::UserRole).toString();
    const QString network_bind_ipv6_address =
        link_network_exit->currentData(Qt::UserRole + 3).toString();

    if (!profile_path.isEmpty()) {
      const QFileInfo profile_info(profile_path);
      if (!profile_info.exists() || !profile_info.isFile()) {
        reject_session_start(
            "Validation failed: runtime profile file is missing",
            QString("Validation failed: runtime profile path does not exist: %1").arg(profile_path),
            "The selected connection profile could not be found.");
        return;
      }
      // Saved profiles cannot silently inherit execution consent across roles.
      redclaw::helper::RuntimeProfileData profile;
      std::string profile_error;
      if (!redclaw::helper::load_runtime_profile_file(
              profile_path.toStdString(), &profile, &profile_error)) {
        reject_session_start("Invalid runtime profile", "Runtime profile validation failed.",
                             "The selected runtime profile could not be parsed.");
        return;
      }
      if (profile.allow_remote_agent.value_or(false)
          && (!host_wait_remote_agent_authorized
              || (!use_form_override && profile.role.value_or(role.toStdString()) != role.toStdString()))) {
        reject_session_start("Local Agent consent required", "Runtime profile Agent consent mismatch.",
                             "Enable Agent permission for this desktop role or disable Agent access in the profile.");
        return;
      }
    }

    if (use_form_override && role == "controller" && transport == "tcp" && host.isEmpty()) {
      reject_session_start(
          "Validation failed: controller+tcp requires target host",
          "Validation failed: target host is required for controller tcp mode.",
          "A target host is required for TCP controller mode.");
      return;
    }

    if (use_form_override && transport == "rendezvous" && rendezvous.isEmpty()) {
      reject_session_start(
          "Validation failed: rendezvous requires URL",
          "Validation failed: rendezvous URL is required for rendezvous transport.",
          "A rendezvous URL is required for this transport.");
      return;
    }

    if (use_form_override && transport == "rendezvous" && !is_valid_session_code(code)) {
      reject_session_start(
          "Validation failed: session code must be 8 chars A-Z0-9",
          "Validation failed: rendezvous session code must be 8 chars using A-Z and 0-9.",
          "The connection code must be 8 letters or numbers.");
      return;
    }

    if (use_form_override && transport == "dht" && !is_valid_session_code(code)) {
      reject_session_start(
          "Validation failed: session code must be 8 chars A-Z0-9",
          "Validation failed: DHT session code must be 8 chars using A-Z and 0-9.",
          "The connection code must be 8 letters or numbers.");
      return;
    }

    if (use_form_override && transport == "sealed-file" && passphrase.isEmpty()) {
      reject_session_start(
          "Validation failed: sealed-file requires signal passphrase",
          "Validation failed: passphrase is required for sealed-file transport.",
          "A passphrase is required for sealed-file transport.");
      return;
    }

    if (use_form_override && transport == "sealed-file" && passphrase.size() < kMinSealedPassphraseLength) {
      reject_session_start(
          "Validation failed: sealed-file passphrase must be at least 16 characters",
          "Validation failed: passphrase must be at least 16 characters for sealed-file transport.",
          "The sealed-file passphrase must be at least 16 characters.");
      return;
    }

    if (use_form_override
        && !network_bind_address.isEmpty()
        && !is_network_bind_address_available(network_bind_address)) {
      reject_session_start(
          "Validation failed: selected network exit is unavailable",
          QString("Validation failed: network bind address is not assigned to an active interface: %1")
              .arg(network_bind_address),
          "The selected network adapter is no longer available. Select another network exit or use Auto.");
      return;
    }

    const ConnectionFlowRole requested_flow_role = role == "host"
        ? ConnectionFlowRole::kHost
        : ConnectionFlowRole::kController;
    const ConnectionFlowSnapshot& current_flow = connection_flow_model.snapshot();
    if (current_flow.view == ConnectionFlowView::kPreConnection
        || current_flow.status == ConnectionFlowStatus::kFailed
        || current_flow.role != requested_flow_role) {
      connection_flow_model.start(requested_flow_role);
    }
    if (requested_flow_role == ConnectionFlowRole::kHost) {
      waiting_page->set_connection_code(code);
      waiting_page->set_remote_control_authorized(host_wait_remote_input_authorized);
      waiting_page->set_remote_agent_authorized(
          host_wait_remote_agent_authorized, agent_settings_dialog->readiness_summary());
    } else {
      connecting_page->set_connection_code(code);
    }
    refresh_connection_flow_page();

    QStringList args;
    args << "--cli";
    if (!profile_path.isEmpty()) {
      args << "--runtime-config" << profile_path;
    }

    if (use_form_override) {
      args << "--role" << role;
      args << "--signal-transport" << transport;
      args << "--signal-timeout-seconds" << QString::number(signal_timeout->value());
      args << "--run-seconds" << QString::number(run_seconds->value());
      if (!network_bind_address.isEmpty()) {
        args << "--network-bind-address" << network_bind_address;
        if (!network_bind_ipv6_address.isEmpty()) {
          args << "--network-bind-ipv6-address" << network_bind_ipv6_address;
        }
      }
      args << "--ice-udp-port" << QString::number(link_ice_udp_port->value());
      ui_settings->setValue("network/ice_udp_port", link_ice_udp_port->value());
      if (enable_port_mapping->isChecked()) {
        args << "--enable-port-mapping";
      }

      const QString signal_dir_value = signal_dir->text().trimmed();
      if (!signal_dir_value.isEmpty()) {
        args << "--signal-dir" << signal_dir_value;
      }

      if (!host.isEmpty()) {
        args << "--target-host" << host;
      }

      if (transport == "tcp" && target_port->value() > 0) {
        args << "--target-port" << QString::number(target_port->value());
      }

      if (!rendezvous.isEmpty()) {
        args << "--rendezvous-url" << rendezvous;
      }

      if ((transport == "rendezvous" || transport == "dht") && !code.isEmpty()) {
        args << "--session-code" << code;
      }

      if (transport == "dht") {
        const QStringList bootstrap_nodes = dht_bootstrap->toPlainText().split('\n');
        for (const QString& node : bootstrap_nodes) {
          const QString trimmed = node.trimmed();
          if (!trimmed.isEmpty()) {
            args << "--dht-bootstrap" << trimmed;
          }
        }
        args << "--dht-poll-interval-ms" << "1000";
        args << "--dht-publish-retry-ms" << "1000";
        if (gui_auto_start.dht_listen_port > 0) {
          args << "--dht-listen-port" << QString::number(gui_auto_start.dht_listen_port);
        }
        if (!enable_ipv6_candidates->isChecked()) {
          args << "--disable-ipv6-candidates";
        }
        args << "--helper-role" << helper_role_combo->currentText();
      }

      if (transport == "sealed-file" && !passphrase.isEmpty()) {
        args << "--signal-passphrase" << passphrase;
      }

      const QStringList servers = resolve_ice_server_selection(
          stun_preset_combo->currentData(Qt::UserRole).toString(),
          additional_ice_servers->toPlainText().split('\n'));
      for (const QString& server : servers) {
        args << "--ice-server" << server;
      }
      if (!gui_auto_start.ice_server_file.isEmpty()) {
        args << "--ice-server-file" << gui_auto_start.ice_server_file;
      }

      if (enable_ice_tcp->isChecked()) {
        args << "--enable-ice-tcp";
      }

      if (stream_smoke->isChecked()) {
        args << "--stream-smoke";
      }
      if (stream_require_capture->isChecked()) {
        args << "--stream-require-capture";
      }
      if (role == "controller" && gui_auto_start.stream_qa_drop_one_media_fragment) {
        args << "--stream-qa-drop-one-media-fragment";
        args << "--stream-qa-incomplete-feedback-class"
             << gui_auto_start.stream_qa_incomplete_feedback_class;
      }
      if (role == "controller"
          && !gui_auto_start.stream_qa_force_required_channel_close.isEmpty()) {
        args << "--stream-qa-force-required-channel-close"
             << gui_auto_start.stream_qa_force_required_channel_close;
      }
      if (gui_auto_start.agent_qa_force_channel_close_after_event) {
        args << "--agent-qa-force-channel-close-after-event";
      }
      if (gui_auto_start.agent_qa_fixture_provider) {
        args << "--agent-qa-fixture-provider";
      }
      if (gui_auto_start.input_diagnostics) args << "--input-diagnostics";
      if (role == "host" && gui_auto_start.stream_qa_native_size) {
        args << "--stream-qa-native-size";
      }
      if (allow_remote_diagnostics->isChecked()) {
        args << "--allow-remote-diagnostics";
      }
      if (role == "host" && host_wait_remote_input_authorized) {
        args << "--allow-remote-input";
      }
      if (host_wait_remote_agent_authorized) {
        QString agent_manifest;
        QString agent_manifest_error;
        if (!write_agent_project_manifest(&agent_manifest, &agent_manifest_error)) {
          reject_session_start(
              "Validation failed: Agent project registration is incomplete",
              "Validation failed: " + agent_manifest_error,
              agent_manifest_error);
          return;
        }
        args << "--allow-remote-agent";
        args << "--agent-project-manifest" << agent_manifest;
      }
      args << "--stream-preview-width" << QString::number(stream_preview_width->value());
      args << "--stream-video-max-width" << QString::number(stream_video_max_width->value());
    }

    if (!gui_auto_start.log_dir.isEmpty()) {
      args << "--log-dir" << gui_auto_start.log_dir;
    }
    if (!gui_auto_start.run_id.isEmpty()) {
      args << "--run-id" << gui_auto_start.run_id;
    }

#if defined(_WIN32)
    direct_frame_pipe_server.stop();
    navigation_frame_pipe_server.stop();
    direct_frame_stream_enabled = false;
    navigation_frame_stream_enabled = false;
    desktop_navigation_panel->set_transport_available(false);
    if (use_form_override && role == "controller" && stream_smoke->isChecked()) {
      const QString pipe_name = make_direct_frame_channel_name();
      QString pipe_error;
      QString decode_device_error;
      ID3D11Device* decode_device = playback_window_canvas->d3d11_decode_device(&decode_device_error);
      direct_frame_pipe_server.configure_d3d11_surface_output(decode_device);
      if (decode_device != nullptr) {
        append_log(session_log, "D3D11 decoded-surface output enabled on the playback device.");
      } else {
        append_log(
            session_log,
            QString("D3D11 decoded-surface output unavailable; CPU frame fallback remains enabled: %1")
                .arg(decode_device_error.isEmpty() ? "non-D3D11 playback backend" : decode_device_error));
      }
      if (!direct_frame_pipe_server.start(pipe_name, &pipe_error)) {
        reject_session_start(
            "Validation failed: live frame channel unavailable",
            QString("Validation failed: direct frame channel could not start: %1").arg(pipe_error),
            "The live display channel could not start. Please try again.");
        return;
      }
      reset_direct_frame_stats();
      direct_frame_stream_enabled = true;
      args << "--stream-frame-pipe" << pipe_name;
      append_log(session_log, QString("Direct frame shared-memory channel ready: %1").arg(pipe_name));

      // Push-render: whenever the shared-memory worker deposits a new frame, invoke
      // handle_direct_frame on the main thread immediately via a queued event.
      // Qt discards the event safely if session_page is destroyed first.
      direct_frame_pipe_server.set_on_frame_notify(
          [&schedule_direct_frame_update]() {
        schedule_direct_frame_update();
          });
      direct_frame_pipe_server.set_on_keyframe_request(
          [session_page, &send_local_keyframe_request](QString reason) {
            QMetaObject::invokeMethod(
                session_page,
                [&send_local_keyframe_request, reason = std::move(reason)]() {
                  send_local_keyframe_request(reason);
                },
                Qt::QueuedConnection);
          });

      const QString navigation_pipe_name = make_navigation_frame_channel_name();
      if (!navigation_frame_pipe_server.start(navigation_pipe_name, &pipe_error)) {
        direct_frame_pipe_server.stop();
        direct_frame_stream_enabled = false;
        reject_session_start(
            "Validation failed: navigation frame channel unavailable",
            QString("Validation failed: navigation frame channel could not start: %1")
                .arg(pipe_error),
            "The desktop navigation channel could not start. Please try again.");
        return;
      }
      navigation_frame_stream_enabled = true;
      args << "--stream-navigation-pipe" << navigation_pipe_name;
      navigation_frame_pipe_server.set_on_frame_notify(
          [&schedule_navigation_frame_update]() {
            schedule_navigation_frame_update();
          });
      append_log(
          session_log,
          QString("Navigation thumbnail shared-memory channel ready: %1")
              .arg(navigation_pipe_name));
    }
#endif

    if (!use_form_override && profile_path.isEmpty()) {
      reject_session_start(
          "Validation failed: role or profile is required",
          "Validation failed: provide role inputs or select a runtime profile.",
          "A connection role or runtime profile is required.");
      return;
    }

    QString start_error;
    const QString program = QCoreApplication::applicationFilePath();
    if (role == "host" && !maintenance_runtime_arguments.isEmpty()) args = std::exchange(maintenance_runtime_arguments, {});
    auto credential_frame = redclaw::security::serialize_local_connection_credential(connection_credential);
    redclaw::security::erase_secret(connection_credential.secret);
    const bool runtime_started = controller.start(program, args, role, credential_frame, &start_error);
    redclaw::security::erase_secret(credential_frame);
    if (!runtime_started) {
      set_link_workflow_running(false);
#if defined(_WIN32)
      direct_frame_pipe_server.stop();
      navigation_frame_pipe_server.stop();
      direct_frame_stream_enabled = false;
      navigation_frame_stream_enabled = false;
      desktop_navigation_panel->set_transport_available(false);
#endif
      status_label->setText("Start failed");
      append_log(session_log, start_error);
      set_process_state("failed");
      set_runtime_banner("The connection service could not start.", "bad");
      set_link_status("The connection service could not start. Please try again.", "bad");
      apply_connection_flow_event(
          ConnectionFlowEvent::kFatalFailure,
          "The connection service could not start. Please retry or exit.");
      debug_status.phase = "failed";
      debug_status.last_error = start_error.toStdString();
      persist_debug_status(true);
      return;
    }

    const std::string saved_run_id = debug_status.run_id;
    const std::string saved_process_log_path = debug_status.process_log_path;
    const std::string saved_status_path = debug_status.status_path;
    const std::string saved_evidence_manifest_path = debug_status.evidence_manifest_path;
    const std::uint64_t saved_app_pid = debug_status.app_pid;
    debug_status = redclaw::diag::DebugRuntimeStatus{};
    debug_status.run_id = saved_run_id;
    debug_status.process_log_path = saved_process_log_path;
    debug_status.status_path = saved_status_path;
    debug_status.evidence_manifest_path = saved_evidence_manifest_path;
    debug_status.app_pid = saved_app_pid;
    debug_status.started_at_unix_ms = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    debug_status.role = role.toStdString();
    debug_status.network_bind_address = network_bind_address.isEmpty()
        ? "auto"
        : network_bind_address.toStdString();
    debug_status.ice_udp_port = static_cast<std::uint16_t>(link_ice_udp_port->value());
    debug_status.phase = "starting";
    debug_status.runtime_running = true;
    debug_status.runtime_pid = static_cast<std::uint64_t>(controller.process().processId());
    persist_debug_status(true);

    status_label->setText("Running");
    set_process_state("running");
    set_connection_state_display("new");
    set_handshake_state(false);
    set_live_link_state(false);
    start_button->setEnabled(false);
    stop_button->setEnabled(true);
    reset_playback_surface(
        "Your remote desktop will appear here",
        role == "controller"
            ? "Connecting to the remote desktop..."
            : "Waiting for the other device to connect.",
        true);
    if (transport == "sealed-file") {
      set_sealed_exchange_state(SealedExchangeState::kWaitingRemote);
      refresh_local_sealed_blob();
    } else {
      set_sealed_exchange_state(SealedExchangeState::kIdle);
    }
    const QString redacted_args = redact_runtime_args_for_log(args);
    set_runtime_banner(role == "host"
                           ? "Your code is ready. Waiting for the other device."
                           : "Connecting to the code you entered.",
                       role == "host" ? "warn" : "info");
    set_link_status(role == "host"
                        ? "This device is now waiting with the code shown above."
                        : "Connecting: waiting for the Host offer from DHT.",
                    role == "host" ? "good" : "info");
    set_link_workflow_running(
        true,
        role == "host" ? "Session in Progress — Waiting" : "Session in Progress — Connecting");
    refresh_connection_flow_page();
    append_log(session_log, QString("Runtime process started: %1 %2").arg(program, redacted_args));
    append_runtime_event(redclaw::render::RuntimeStatusSeverity::kInfo,
                         "process",
               QString("Runtime process started: %1 %2").arg(program, redacted_args).toStdString());
  });

  QObject::connect(stop_button, &QPushButton::clicked, [&]() {
    manual_runtime_stop_requested = true;
    connection_flow_model.begin_stopping();
    refresh_connection_flow_page();
#if defined(_WIN32)
    direct_frame_pipe_server.stop();
    navigation_frame_pipe_server.stop();
    direct_frame_stream_enabled = false;
    navigation_frame_stream_enabled = false;
    desktop_navigation_panel->set_transport_available(false);
#endif
    controller.request_stop(!debug_reconnect_requested);
    status_label->setText("Stopping");
    set_process_state("stopping");
    set_live_link_state(false);
    set_runtime_banner("Ending the current connection.", "warn");
    set_link_status("Ending the current connection.", "warn");
    reset_playback_surface(
        "Your remote desktop will appear here",
        "Connection is closing.",
        true);
    append_log(session_log, "Stop signal sent to runtime process.");
    persist_debug_status(true);
  });

  const auto retry_active_flow = [&]() {
    if (controller.is_running()) {
      debug_reconnect_requested = true;
      stop_button->click();
      return;
    }
    apply_connection_flow_event(ConnectionFlowEvent::kAutomaticRetry);
    start_button->click();
  };
  const auto exit_active_flow = [&]() {
    if (connection_flow_model.snapshot().status == ConnectionFlowStatus::kStopping) {
      return;
    }
    connection_flow_model.begin_stopping();
    refresh_connection_flow_page();
    reset_playback_surface(
        "Your remote desktop will appear here",
        "Connection is closing.",
        true);
    if (controller.is_running()) {
      stop_button->click();
    } else {
      apply_connection_flow_event(ConnectionFlowEvent::kExitCompleted);
      set_link_workflow_running(false);
    }
  };
  connecting_page->set_on_retry(retry_active_flow);
  waiting_page->set_on_retry(retry_active_flow);
  connecting_page->set_on_exit(exit_active_flow);
  waiting_page->set_on_exit(exit_active_flow);

  auto prepare_evidence_export = [&](std::shared_ptr<QJsonObject> exported) -> GuiDiagnosticWriter::Work {
    QJsonObject manifest;
    manifest.insert("schema", "redclaw.debug.evidence.manifest.v1");
    manifest.insert("generated_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    manifest.insert("run_id", gui_auto_start.run_id);
    manifest.insert("role", QString::fromStdString(debug_status.role));
    manifest.insert("app_pid", static_cast<qint64>(debug_status.app_pid));
    manifest.insert("runtime_pid", static_cast<qint64>(debug_status.runtime_pid));
    manifest.insert("ice_udp_port", static_cast<int>(debug_status.ice_udp_port));
    const QStringList selected_ice_servers = resolve_ice_server_selection(
        stun_preset_combo->currentData(Qt::UserRole).toString(),
        additional_ice_servers->toPlainText().split('\n'));
    const QString ice_server_file = gui_auto_start.ice_server_file;
    auto status_work = debug_runtime_status_work(debug_status);
    return [manifest, status_work, exported, selected_ice_servers, ice_server_file, effective_log_dir,
        evidence_manifest_path, process_logger](QString* export_error) mutable {
    manifest.insert("status", status_work());
    if (process_logger) {
      process_logger->flush();
      if (!process_logger->is_healthy()) { *export_error = "process log flush failed"; return false; }
    }
    const QString snapshot_directory = QDir(effective_log_dir).filePath("evidence");
    if (!QDir().mkpath(snapshot_directory)) {
      if (export_error != nullptr) {
        *export_error = "failed to create evidence snapshot directory";
      }
      return false;
    }

    QJsonArray files;
    QDir log_directory(effective_log_dir);
    const QFileInfoList entries = log_directory.entryInfoList(
        QDir::Files | QDir::Readable,
        QDir::Time | QDir::Reversed);
    for (const QFileInfo& entry : entries) {
      if (entry.absoluteFilePath() == QFileInfo(evidence_manifest_path).absoluteFilePath()) {
        continue;
      }
      const QString snapshot_path = QDir(snapshot_directory).filePath(entry.fileName());
      QString copy_error;
      if (!copy_file_atomically(entry.absoluteFilePath(), snapshot_path, &copy_error)) {
        if (export_error != nullptr) {
          *export_error = QString("failed to snapshot %1: %2").arg(entry.fileName(), copy_error);
        }
        return false;
      }
      const QFileInfo snapshot(snapshot_path);
      QJsonObject file;
      file.insert("name", snapshot.fileName());
      file.insert("source_path", entry.absoluteFilePath());
      file.insert("path", snapshot.absoluteFilePath());
      file.insert("size", snapshot.size());
      file.insert("sha256", sha256_file_hex(snapshot.absoluteFilePath()));
      files.append(file);
    }

    manifest.insert("ice_server_count", selected_ice_servers.size()
        + count_ice_server_file_entries(ice_server_file));
    manifest.insert("files", files);
    if (!write_json_atomically(evidence_manifest_path, manifest, export_error)) {
      return false;
    }
    exported->insert("manifest_path", evidence_manifest_path);
    exported->insert("manifest_sha256", sha256_file_hex(evidence_manifest_path));
    return true;
    };
  };

  QLocalServer debug_control_server(&window);
  if (gui_auto_start.enable_debug_control) {
    debug_control_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!debug_control_server.listen(gui_auto_start.debug_control_name)) {
      if (error_detail != nullptr) {
        *error_detail = QString("debug control listen failed name=%1 error=%2")
            .arg(gui_auto_start.debug_control_name, debug_control_server.errorString())
            .toStdString();
      }
      g_diagnostic_writer = nullptr;
      return false;
    }

    append_log(
        session_log,
        QString("Debug control ready: name=%1 access=current-user").arg(gui_auto_start.debug_control_name));
    QObject::connect(&debug_control_server, &QLocalServer::newConnection, [&]() {
      while (QLocalSocket* socket = debug_control_server.nextPendingConnection()) {
        socket->setParent(&debug_control_server);
        socket->setProperty("request_buffer", QByteArray());
        socket->setProperty("request_complete", false);
        const auto handle_debug_control_request = [&, socket]() {
          if (socket->property("request_complete").toBool()) {
            return;
          }
          QByteArray buffer = socket->property("request_buffer").toByteArray();
          buffer.append(socket->readAll());
          if (buffer.size() > 64 * 1024) {
            socket->setProperty("request_complete", true);
            QJsonObject response;
            response.insert("schema", "redclaw.debug-control.v1");
            response.insert("request_id", "unknown");
            response.insert("ok", false);
            response.insert("error_code", "request_too_large");
            response.insert("error_detail", "request exceeds 65536 bytes");
            response.insert("status", debug_runtime_status_json(debug_status));
            socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
            socket->flush();
            socket->disconnectFromServer();
            return;
          }
          const qsizetype newline = buffer.indexOf('\n');
          if (newline < 0) {
            socket->setProperty("request_buffer", buffer);
            return;
          }

          const QByteArray request_payload = buffer.left(newline).trimmed();
          socket->setProperty("request_complete", true);
          const DebugControlParseResult parsed = parse_debug_control_request(request_payload);
          const QString request_id = parsed.request.request_id;
          const QString action = parsed.action_name.isEmpty() ? "unknown" : parsed.action_name;
          bool ok = parsed.ok;
          QString error_code = parsed.error_code;
          QString control_error = parsed.error_detail;
          QJsonObject result;
          GuiDiagnosticWriter::Work deferred_work;
          std::shared_ptr<QJsonObject> deferred_result;

          auto fail = [&](const QString& code, const QString& detail) {
            ok = false;
            error_code = code;
            control_error = detail;
          };

          if (parsed.ok) {
            switch (parsed.request.action) {
            case DebugControlAction::kExit:
              result.insert("exit_pending", true);
              break;
            case DebugControlAction::kInputProbeStart:
            case DebugControlAction::kInputProbeStop:
            case DebugControlAction::kInputProbeExport: {
#if defined(_WIN32) && !defined(NDEBUG)
              if (!gui_auto_start.agent_qa_fixture_provider && !gui_auto_start.input_diagnostics) {
                fail("fixture_required", "Input evidence requires explicit input diagnostics or a local fixture."); break;
              }
              if (parsed.request.action == DebugControlAction::kInputProbeExport) {
                redclaw::protocol::StreamControlMessageV1 command;
                command.type = redclaw::protocol::StreamControlMessageTypeV1::kInputReleaseAll;
                command.message_id = ++gui_control_message_id; command.session_epoch = "local";
                command.sent_at_ms = QDateTime::currentMSecsSinceEpoch(); command.input_sequence = gui_monotonic_us();
                if (!controller.send_control_message(command, &control_error)) fail("input_export_failed", control_error);
                result.insert("generation", qint64(command.input_sequence));
              }
              if (gui_auto_start.input_diagnostics) {
                if (input_diagnostic_target) {
                  if (parsed.request.action == DebugControlAction::kInputProbeStart) input_diagnostic_target->start();
                  else input_diagnostic_target->stop();
                  result.insert("input_target", input_diagnostic_target->snapshot());
                  if (parsed.request.action == DebugControlAction::kInputProbeExport) {
                    const auto path = QDir(effective_log_dir).filePath("input-target-" + QString::number(result.value("generation").toInteger()) + ".json");
                    deferred_work = input_diagnostic_target->export_work(path);
                    result.insert("receipt_path", path);
                  }
                }
                break;
              }
              if (role_combo->currentText() == "host") break;
              if (!qa_input_probe) { fail("fixture_controller_required", "Native target probe is available on the fixture Controller."); }
              else {
                if (parsed.request.action == DebugControlAction::kInputProbeStart && !qa_input_probe->start(&control_error)) fail("input_probe_start_failed", control_error);
                if (parsed.request.action != DebugControlAction::kInputProbeStart) qa_input_probe->stop();
                result.insert("input_probe", qa_input_probe->snapshot());
                if (parsed.request.action == DebugControlAction::kInputProbeExport) {
                  const auto receipt_path = QDir(effective_log_dir).filePath("input-receipts-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json");
                  deferred_work = qa_input_probe->export_work(receipt_path);
                  result.insert("receipt_path", receipt_path);
                }
              }
#else
              fail("debug_windows_required", "Native input QA is enabled only in local Windows Debug fixtures.");
#endif
              break;
            }
            case DebugControlAction::kMeasurementArm:
              app.latency.arm_trace();
              result.insert("measurement", app.latency.snapshot().value("trace"));
              break;
            case DebugControlAction::kMeasurementExport: {
              const auto trace_path = QDir(effective_log_dir).filePath(
                  "gui-trace-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".bin");
              deferred_work = app.latency.trace_export_work(trace_path);
              result.insert("trace_path", trace_path);
              result.insert("measurement", app.latency.snapshot().value("trace"));
              break;
            }
            case DebugControlAction::kStatus: {
              const auto& flow = connection_flow_model.snapshot();
              result.insert("connection_flow", QJsonObject{
                  {"transport_ready", flow.transport_ready},
                  {"complete", flow.status == ConnectionFlowStatus::kComplete},
                  {"attempt", flow.attempt},
                  {"phase_text", flow.role == ConnectionFlowRole::kHost
                      ? waiting_page->phase_text() : connecting_page->phase_text()}});
              ok = true;
              break;
            }
            case DebugControlAction::kStart:
              if (controller.is_running()) {
                fail("already_running", "runtime process is already running");
              } else if (!gui_auto_start.role.isEmpty()
                  && parsed.request.role != gui_auto_start.role) {
                fail("role_mismatch", "requested role does not match the supervisor configuration");
              } else {
                role_combo->setCurrentText(parsed.request.role);
                host_wait_remote_input_authorized =
                    parsed.request.role == "host"
                    && connection_entry_page->allow_remote_control_checkbox()->isChecked();
                host_wait_remote_agent_authorized = (parsed.request.role == "host"
                    ? connection_entry_page->allow_remote_agent_checkbox()
                    : connection_entry_page->allow_controller_agent_checkbox())->isChecked();
                debug_status.role = parsed.request.role.toStdString();
                start_button->click();
                if (!controller.is_running()) {
                  fail("start_failed", "runtime process did not start; inspect local log");
                } else {
                  ok = true;
                }
              }
              break;
            case DebugControlAction::kReconnect:
              if (!controller.is_running()) {
                fail("not_running", "runtime process is not running");
              } else {
                debug_reconnect_requested = true;
                stop_button->click();
                ok = true;
              }
              break;
            case DebugControlAction::kStop:
              if (!controller.is_running()) {
                fail("not_running", "runtime process is not running");
              } else {
                stop_button->click();
                ok = true;
              }
              break;
            case DebugControlAction::kTailLog: {
              const auto started_at = std::chrono::steady_clock::now();
              if (process_logger == nullptr || !process_logger->is_open()) {
                fail("log_unavailable", "the bounded process log ring is unavailable");
                break;
              }
              constexpr std::size_t kTailLogMaxBytes = 64U * 1024U;
              const auto snapshot = process_logger->snapshot(
                  0,
                  static_cast<std::size_t>(parsed.request.tail_limit),
                  kTailLogMaxBytes);
              QJsonArray lines;
              for (const std::string& line : snapshot.lines) {
                lines.append(QString::fromStdString(line));
              }
              constexpr qsizetype kTailLogJsonBudget = 48 * 1024;
              bool response_trimmed = false;
              QByteArray serialized_lines = QJsonDocument(lines).toJson(QJsonDocument::Compact);
              while (!lines.isEmpty() && serialized_lines.size() > kTailLogJsonBudget) {
                lines.removeAt(0);
                response_trimmed = true;
                serialized_lines = QJsonDocument(lines).toJson(QJsonDocument::Compact);
              }
              const std::uint64_t available_lines = snapshot.next_cursor != 0
                  && snapshot.next_cursor >= snapshot.first_cursor
                  ? snapshot.next_cursor - snapshot.first_cursor + 1U
                  : 0U;
              const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - started_at).count();
              result.insert("lines", lines);
              result.insert(
                  "truncated",
                  response_trimmed || snapshot.lines.size() < available_lines);
              result.insert("response_bytes", static_cast<qint64>(serialized_lines.size()));
              result.insert("elapsed_ms", static_cast<qint64>(elapsed_ms));
              ok = true;
              break;
            }
            case DebugControlAction::kRemoteLogSnapshot: {
              if (!controller.is_running()) {
                fail("not_running", "runtime process is not running");
              } else if (role_combo->currentText() != "controller") {
                fail("role_mismatch", "remote log snapshots can only be requested by a Controller");
              } else if (!debug_status.channel_open) {
                fail("channel_not_open", "the reliable control channel is not open");
              } else {
                QString peer_request_id;
                if (!send_remote_log_request(
                        redclaw::protocol::RemoteLogModeV1::kSnapshot,
                        &peer_request_id,
                        &control_error)) {
                  error_code = "remote_log_request_failed";
                  ok = false;
                } else {
                  result.insert("peer_request_id", peer_request_id);
                  ok = true;
                }
              }
              break;
            }
            case DebugControlAction::kRemoteLogRead: {
              if (!debug_remote_log_snapshot.active()) {
                fail("remote_log_not_requested", "no remote log snapshot has been requested");
                break;
              }
              QJsonArray lines;
              for (const QString& line : debug_remote_log_snapshot.tail_lines(parsed.request.tail_limit)) {
                lines.append(QString::fromStdString(
                    redclaw::diag::redact_log_text(line.toStdString())));
              }
              result.insert("peer_request_id", debug_remote_log_snapshot.request_id());
              result.insert("complete", debug_remote_log_snapshot.is_complete());
              result.insert(
                  "remote_error",
                  QString::fromStdString(redclaw::diag::redact_log_text(
                      debug_remote_log_snapshot.error_detail().toStdString())));
              result.insert("lines", lines);
              ok = true;
              break;
            }
            case DebugControlAction::kRemoteControlStart: {
              if (!controller.is_running()) {
                fail("not_running", "runtime process is not running");
              } else if (role_combo->currentText() != "controller") {
                fail("role_mismatch", "remote control can only be started by a Controller");
              } else if (!debug_status.channel_open) {
                fail("channel_not_open", "the reliable control channel is not open");
              } else if (!remote_control_button->isEnabled()) {
                fail(
                    "remote_control_unavailable",
                    remote_control_status->text());
              } else {
                playback_window->show();
                playback_window->raise();
                playback_window->activateWindow();
#if defined(_WIN32)
                (void)SetForegroundWindow(
                    reinterpret_cast<HWND>(playback_window->winId()));
#endif
                remote_control_button->click();
                result.insert("request_pending", remote_input_request_pending);
                result.insert("active", remote_input_capture->active());
                ok = remote_input_request_pending || remote_input_capture->active();
                if (!ok) {
                  fail("remote_control_start_failed", remote_control_status->text());
                }
              }
              break;
            }
            case DebugControlAction::kRemoteControlPause:
              if (role_combo->currentText() != "controller") {
                fail("role_mismatch", "remote control can only be paused by a Controller");
              } else if (!remote_input_capture->active()) {
                fail("remote_control_not_active", "remote control is not active");
              } else {
                remote_control_button->click();
                result.insert("active", remote_input_capture->active());
                ok = !remote_input_capture->active();
              }
              break;
            case DebugControlAction::kRemoteInputMouseClick: {
              redclaw::protocol::RemoteInputMouseButtonV1 button =
                  redclaw::protocol::RemoteInputMouseButtonV1::kLeft;
              if (parsed.request.mouse_button == "right") {
                button = redclaw::protocol::RemoteInputMouseButtonV1::kRight;
              } else if (parsed.request.mouse_button == "middle") {
                button = redclaw::protocol::RemoteInputMouseButtonV1::kMiddle;
              } else if (parsed.request.mouse_button == "x1") {
                button = redclaw::protocol::RemoteInputMouseButtonV1::kX1;
              } else if (parsed.request.mouse_button == "x2") {
                button = redclaw::protocol::RemoteInputMouseButtonV1::kX2;
              }
              const std::uint64_t batches_before = remote_input_capture->sent_batch_count();
              if (!remote_input_capture->submit_qa_mouse_click(
                      parsed.request.normalized_x,
                      parsed.request.normalized_y,
                      button,
                      &control_error)) {
                error_code = "remote_mouse_click_failed";
                ok = false;
              } else {
                result.insert(
                    "sent_batch_delta",
                    static_cast<qint64>(remote_input_capture->sent_batch_count() - batches_before));
                result.insert("queued_critical_events", static_cast<qint64>(
                    remote_input_capture->queued_critical_event_count()));
                ok = true;
              }
              break;
            }
            case DebugControlAction::kRemoteInputKeyPress: {
              const std::uint64_t batches_before = remote_input_capture->sent_batch_count();
              if (!remote_input_capture->submit_qa_key_press(
                      parsed.request.scan_code,
                      parsed.request.virtual_key,
                      parsed.request.extended,
                      &control_error)) {
                error_code = "remote_key_press_failed";
                ok = false;
              } else {
                result.insert(
                    "sent_batch_delta",
                    static_cast<qint64>(remote_input_capture->sent_batch_count() - batches_before));
                result.insert("queued_critical_events", static_cast<qint64>(
                    remote_input_capture->queued_critical_event_count()));
                ok = true;
              }
              break;
            }
            case DebugControlAction::kAgentStatus:
#if defined(_WIN32)
              {
                const auto transport = direct_frame_pipe_server.stats_snapshot();
                auto playback = playback_window_canvas->diagnostic_snapshot();
                playback.insert("decoded_surface_frames", qint64(transport.d3d11_decode_surface_frames));
                playback.insert("surface_fallbacks", qint64(transport.d3d11_surface_fallbacks));
                playback.insert("cpu_transfer_frames", qint64(transport.hardware_decode_cpu_transfer_frames));
                playback.insert("software_frames", qint64(transport.software_decode_bgra_frames));
                playback.insert("decode_failures", qint64(transport.decode_failures));
                playback.insert("present_failures", qint64(direct_frame_present_failures));
                playback.insert("content_width", remote_input_capture->remote_frame_size().width());
                playback.insert("content_height", remote_input_capture->remote_frame_size().height());
                result.insert("playback", playback);
              }
#endif
#if defined(_WIN32) && !defined(NDEBUG)
              if (qa_input_probe) result.insert("input_probe", qa_input_probe->snapshot());
#endif
              result.insert("task_timing", agent_panel->diagnostic_timing());
              result.insert("runtime_stdio", runtime_stdio_reader->snapshot());
              result.insert("task_id", agent_panel->current_task_id());
              result.insert("task_state", QString::fromLatin1(
                  redclaw::protocol::to_string(agent_panel->current_task_state()).data()));
              result.insert("status", agent_panel->status_text());
              result.insert("provider_count", agent_panel->provider_count());
              result.insert("project_count", agent_panel->project_count());
              result.insert("approval_pending", agent_panel->approval_pending());
              {
                QJsonArray providers;
                for (const auto& view : agent_panel->providers()) {
                  QJsonObject provider;
                  provider.insert("name", view.label);
                  provider.insert("kind", static_cast<int>(view.kind));
                  provider.insert("available", view.available);
                  provider.insert("readiness", static_cast<int>(view.readiness));
                  provider.insert("version", view.display_name);
                  provider.insert("reason",
                      QString::fromStdString(redclaw::diag::redact_log_text(
                          view.reason.toStdString())));
                  QJsonArray models;
                  for (const auto& model : view.models) {
                    models.append(model);
                  }
                  provider.insert("models", models);
                  providers.append(provider);
                }
                result.insert("providers", providers);
                QJsonArray projects;
                for (const auto& view : agent_panel->projects()) {
                  QJsonObject project;
                  project.insert("id", view.id);
                  project.insert("name", view.name);
                  project.insert("git_repository", view.git_repository);
                  projects.append(project);
                }
                result.insert("projects", projects);
              }
              ok = true;
              break;
            case DebugControlAction::kAgentStartFixture: {
              if (agent_panel->provider_count() == 0
                         || agent_panel->project_count() == 0) {
                fail("agent_unavailable", "Agent provider and project catalog must be ready");
              } else {
                const auto requested_provider = parsed.request.agent_provider == "cursor"
                    ? redclaw::protocol::AgentProviderKindV1::kCursor
                    : redclaw::protocol::AgentProviderKindV1::kCodex;
                if (!agent_panel->select_provider(requested_provider)) {
                  fail("agent_provider_unavailable",
                      parsed.request.agent_provider + " provider is not ready");
                  break;
                }
                if (parsed.request.fixture_id == "write_marker_and_test") {
                  if (!agent_panel->selected_project_is_git()) {
                    fail("agent_project_not_git",
                        "write_marker_and_test requires a registered Git project");
                    break;
                  }
                  agent_panel->select_isolated_worktree();
                }
                const QString prior_task = agent_panel->current_task_id();
                // Exercise the same modeless Host entry as a local user. Merely
                // reading Agent status never changes window visibility.
                if (role_combo->currentText() == "host") {
                  agent_panel_presentation->open_button()->click();
                }
                agent_panel->set_instruction_text(
                    parsed.request.fixture_id == "inspect_project"
                    ? "Inspect the registered project and report its current branch, dirty state, and focused test entrypoints. Do not modify files."
                    : "In the isolated worktree, create redclaw-agent-fixture.txt containing the single line RedClaw Agent fixture. If verify-agent-fixture.ps1 exists, run it with PowerShell. Then run git status --short and report the result. Do not commit or push.");
                QString submit_error;
                if (!agent_panel->trigger_submit(AgentSubmitMode::kCreateTask, &submit_error)
                    || agent_panel->current_task_id().isEmpty()
                    || agent_panel->current_task_id() == prior_task) {
                  fail("agent_fixture_start_failed",
                      submit_error.isEmpty() ? agent_panel->status_text() : submit_error);
                } else {
                  result.insert("task_id", agent_panel->current_task_id());
                  result.insert("fixture_id", parsed.request.fixture_id);
                  result.insert("provider", parsed.request.agent_provider);
                  ok = true;
                }
              }
              break;
            }
            case DebugControlAction::kAgentFollowUpFixture:
              if (agent_panel->current_task_id().isEmpty()) {
                fail("agent_task_missing", "no RedClaw Agent task is selected");
              } else {
                agent_panel->set_instruction_text(
                    "Verify redclaw-agent-fixture.txt still contains exactly the expected single line. Run verify-agent-fixture.ps1 with PowerShell if it exists, then report the result without modifying, committing, or pushing anything.");
                QString submit_error;
                ok = agent_panel->trigger_submit(AgentSubmitMode::kStartTurn, &submit_error);
                result.insert("task_id", agent_panel->current_task_id());
                if (!ok) {
                  fail("agent_follow_up_failed",
                      submit_error.isEmpty() ? agent_panel->status_text() : submit_error);
                }
              }
              break;
            case DebugControlAction::kAgentInterrupt:
              if (agent_panel->current_task_id().isEmpty()) {
                fail("agent_task_missing", "no RedClaw Agent task is selected");
              } else {
                QString interrupt_error;
                ok = agent_panel->trigger_interrupt(&interrupt_error);
                result.insert("task_id", agent_panel->current_task_id());
                if (!ok) {
                  fail("agent_interrupt_failed", interrupt_error);
                }
              }
              break;
            case DebugControlAction::kAgentApproval:
              if (!agent_panel->approval_pending()) {
                fail("agent_approval_missing", "no Agent approval is pending");
              } else {
                const QString approval_id = agent_panel->approval_request_id();
                QString approval_error;
                ok = agent_panel->trigger_approval(parsed.request.approval_decision == "accept"
                    ? redclaw::protocol::AgentApprovalDecisionV1::kAccept
                    : redclaw::protocol::AgentApprovalDecisionV1::kReject,
                    &approval_error);
                result.insert("approval_request_id", approval_id);
                result.insert("decision", parsed.request.approval_decision);
                if (!ok) {
                  error_code = "agent_approval_failed";
                  control_error = approval_error;
                }
              }
              break;
            case DebugControlAction::kAgentEvidence: {
              result.insert("task_id", agent_panel->current_task_id());
              result.insert("status", agent_panel->status_text());
              result.insert("approval_pending", agent_panel->approval_pending());
              result.insert("provider_count", agent_panel->provider_count());
              result.insert("project_count", agent_panel->project_count());
              result.insert("telemetry",
                  debug_runtime_status_json(debug_status).value("agent").toObject());
              QJsonArray tail;
              for (const QString& line : agent_panel->bounded_event_tail(100)) {
                tail.append(line);
              }
              result.insert("event_tail", tail);
              ok = true;
              break;
            }
            case DebugControlAction::kExportEvidence: {
              deferred_result = std::make_shared<QJsonObject>();
              deferred_work = prepare_evidence_export(deferred_result);
              break;
            }
            }
          }

          if (parsed.ok) {
            const auto input_state_name = redclaw::protocol::to_string(remote_input_state);
            const auto input_reason_name = redclaw::protocol::to_string(remote_input_reason);
            const QRect content = remote_input_capture->content_rect();
            QJsonObject remote_input_result;
            remote_input_result.insert("supported", remote_input_supported);
            remote_input_result.insert("authorized", remote_input_authorized);
            remote_input_result.insert("frame_ready", remote_input_frame_ready);
            remote_input_result.insert("request_pending", remote_input_request_pending);
            remote_input_result.insert("active", remote_input_capture->active());
            remote_input_result.insert("control_enabled", remote_input_capture->control_enabled());
            remote_input_result.insert("input_forwarding", remote_input_capture->input_forwarding());
            remote_input_result.insert(
                "local_suspension_reason",
                remote_input_capture->local_suspension_reason());
            remote_input_result.insert("button_enabled", remote_control_button->isEnabled());
            remote_input_result.insert(
                "keyboard_target_active",
                remote_input_capture->keyboard_target_is_active());
            remote_input_result.insert(
                "state",
                QString::fromLatin1(
                    input_state_name.data(), static_cast<qsizetype>(input_state_name.size())));
            remote_input_result.insert(
                "reason",
                QString::fromLatin1(
                    input_reason_name.data(), static_cast<qsizetype>(input_reason_name.size())));
            remote_input_result.insert("content_x", content.x());
            remote_input_result.insert("content_y", content.y());
            remote_input_result.insert("content_width", content.width());
            remote_input_result.insert("content_height", content.height());
            remote_input_result.insert(
                "sent_batches",
                static_cast<qint64>(remote_input_capture->sent_batch_count()));
            remote_input_result.insert(
                "mouse_loopback_suppressed",
                static_cast<qint64>(remote_input_capture->suppressed_mouse_loopback_count()));
            remote_input_result.insert(
                "blocked_control_click_hint_total",
                static_cast<qint64>(remote_input_capture->blocked_control_click_hint_count()));
            remote_input_result.insert(
                "application_ack_rtt_ms",
                static_cast<qint64>(remote_input_capture->last_application_ack_rtt_ms()));
            result.insert("remote_input", remote_input_result);
            if (gui_auto_start.input_diagnostics) {
              QJsonArray counts;
              for (const auto count : remote_input_capture->sent_event_counts()) counts.append(qint64(count));
              result.insert("input_sent_event_counts", counts);
#if defined(_WIN32) && !defined(NDEBUG)
              if (input_diagnostic_target) result.insert("input_target", input_diagnostic_target->snapshot());
#endif
            }
          }

          debug_status.runtime_running = controller.is_running();
          debug_status.runtime_pid = controller.is_running()
              ? static_cast<std::uint64_t>(controller.process().processId())
              : 0;
          persist_debug_status(true);
          append_control_event(request_id, action, ok, error_code);
          diagnostic_writer.post_log({"debug-control",
                QString("request_id=%1 action=%2 ok=%3 error_code=%4")
                    .arg(request_id, action, ok ? "true" : "false", error_code)
                    .toUtf8(), !ok});

          QJsonObject response;
          response.insert("schema", "redclaw.debug-control.v1");
          response.insert("request_id", request_id);
          response.insert("ok", ok);
          response.insert("error_code", error_code);
          response.insert(
              "error_detail",
              QString::fromStdString(redclaw::diag::redact_log_text(control_error.toStdString())));
          auto response_status_work = debug_runtime_status_work(debug_status);
          auto response_status = std::make_shared<QJsonObject>();
          const auto action_work = std::move(deferred_work);
          deferred_work = [action_work, response_status_work, response_status](QString* error) {
            *response_status = response_status_work();
            return !action_work || action_work(error);
          };
          response.insert("result", result);
          auto send_response = [guard = QPointer<QLocalSocket>(socket), response, response_status, deferred_result, &diagnostic_writer,
              exit_requested = parsed.ok && parsed.request.action == DebugControlAction::kExit](
              bool written, const QString& error) mutable {
            if (!guard) return;
            auto status = *response_status;
            status.insert("diagnostic_writer", diagnostic_writer.snapshot()); response.insert("status", status);
            if (deferred_result) {
              auto result = response["result"].toObject();
              for (auto it = deferred_result->begin(); it != deferred_result->end(); ++it) result.insert(it.key(), it.value());
              response.insert("result", result);
            }
            if (!written) {
              response.insert("ok", false); response.insert("error_code", "diagnostic_barrier_failed");
              response.insert("error_detail", QString::fromStdString(redclaw::diag::redact_log_text(error.toStdString())));
            }
            guard->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
            guard->flush(); guard->disconnectFromServer();
            if (exit_requested) QTimer::singleShot(0, QCoreApplication::instance(), [] { QCoreApplication::quit(); });
          };
          const bool critical_state = parsed.ok && (parsed.request.action == DebugControlAction::kStop
              || parsed.request.action == DebugControlAction::kStart || parsed.request.action == DebugControlAction::kReconnect);
          if (deferred_work || critical_state) {
            if (!diagnostic_writer.after_pending(std::move(deferred_work), send_response))
              send_response(false, "diagnostic completion queue is full");
          } else send_response(true, {});
        };
        QObject::connect(socket, &QLocalSocket::readyRead, handle_debug_control_request);
        QTimer::singleShot(0, socket, handle_debug_control_request);
      }
    });
  }

  auto apply_gui_auto_start = [&]() {
    if (!gui_auto_start.role.isEmpty()) {
      role_combo->setCurrentText(gui_auto_start.role);
    }
    if (!gui_auto_start.transport.isEmpty()) {
      transport_combo->setCurrentText(gui_auto_start.transport);
    }
    if (!gui_auto_start.target_host.isEmpty()) {
      target_host->setText(gui_auto_start.target_host);
    }
    if (gui_auto_start.target_port > 0) {
      target_port->setValue(gui_auto_start.target_port);
    }
    if (!gui_auto_start.rendezvous_url.isEmpty()) {
      rendezvous_url->setText(gui_auto_start.rendezvous_url);
    }
    if (!gui_auto_start.session_code.isEmpty()) {
      session_code->setText(gui_auto_start.session_code);
      connection_entry_page->set_local_code(gui_auto_start.session_code);
      connection_entry_page->set_peer_code(gui_auto_start.session_code);
    }
    if (!gui_auto_start.signal_dir.isEmpty()) {
      signal_dir->setText(gui_auto_start.signal_dir);
    }
    if (!gui_auto_start.signal_passphrase.isEmpty()) {
      signal_passphrase->setText(gui_auto_start.signal_passphrase);
    }
    run_seconds->setValue(static_cast<int>(gui_auto_start.run_seconds));
    signal_timeout->setValue(static_cast<int>(gui_auto_start.signal_timeout_seconds));
    stream_smoke->setChecked(gui_auto_start.stream_smoke);
    stream_require_capture->setChecked(gui_auto_start.stream_require_capture);
    stream_preview_width->setValue(static_cast<int>(gui_auto_start.stream_preview_width));
    stream_video_max_width->setValue(static_cast<int>(gui_auto_start.stream_video_max_width));
    enable_ice_tcp->setChecked(gui_auto_start.enable_ice_tcp);
    enable_port_mapping->setChecked(gui_auto_start.enable_port_mapping);
    QString configured_stun_preset = match_stun_server_preset(gui_auto_start.ice_servers);
    if (gui_auto_start.ice_servers.isEmpty() && !gui_auto_start.ice_server_file.isEmpty()) {
      configured_stun_preset = custom_stun_server_preset_id();
    }
    const int configured_stun_index = stun_preset_combo->findData(configured_stun_preset);
    stun_preset_combo->setCurrentIndex(configured_stun_index >= 0 ? configured_stun_index : 0);
    additional_ice_servers->setPlainText(
        configured_stun_preset == custom_stun_server_preset_id()
            ? gui_auto_start.ice_servers.join('\n')
            : QString());
    link_enable_port_mapping->setChecked(gui_auto_start.enable_port_mapping || link_enable_port_mapping->isChecked());
    if (!gui_auto_start.agent_project_roots.isEmpty()) {
      QString project_error;
      if (!agent_settings_dialog->replace_project_roots_for_automation(
              gui_auto_start.agent_project_roots, &project_error)) {
        append_log(session_log, "GUI Agent project automation failed: " + project_error);
        QCoreApplication::exit(2);
        return;
      }
    }
    if (gui_auto_start.allow_remote_agent) {
      (gui_auto_start.role == "host"
          ? connection_entry_page->allow_remote_agent_checkbox()
          : connection_entry_page->allow_controller_agent_checkbox())->setChecked(true);
    }
    if (gui_auto_start.auto_start) {
      host_wait_remote_input_authorized =
          role_combo->currentText() == "host"
          && connection_entry_page->allow_remote_control_checkbox()->isChecked();
      host_wait_remote_agent_authorized = (role_combo->currentText() == "host"
          ? connection_entry_page->allow_remote_agent_checkbox()
          : connection_entry_page->allow_controller_agent_checkbox())->isChecked();
      append_log(
          session_log,
          QString("GUI auto-start armed: role=%1 transport=%2 session_code=%3 signal_dir=%4")
              .arg(role_combo->currentText(),
                   transport_combo->currentText(),
                   session_code->text().trimmed(),
                   signal_dir->text().trimmed()));
      QTimer::singleShot(0, &window, [start_button]() {
        start_button->click();
      });
    }
  };

  window.setCentralWidget(shell);

  auto animate_panel_entrance = [&](QWidget* widget, int delay_ms) {
    auto* effect = new QGraphicsOpacityEffect(widget);
    effect->setOpacity(0.0);
    widget->setGraphicsEffect(effect);

    QTimer::singleShot(delay_ms, &window, [effect, widget]() {
      auto* animation = new QPropertyAnimation(effect, "opacity", widget);
      animation->setDuration(360);
      animation->setStartValue(0.0);
      animation->setEndValue(1.0);
      animation->setEasingCurve(QEasingCurve::OutCubic);
      animation->start(QAbstractAnimation::DeleteWhenStopped);
    });
  };

  if (QScreen* screen = app.primaryScreen()) {
    const QRect available_geometry = screen->availableGeometry();
    const QSize window_size = window.size();
    window.move(
        available_geometry.x() + (available_geometry.width() - window_size.width()) / 2,
        available_geometry.y() + (available_geometry.height() - window_size.height()) / 2);
  }

  window.show();
  animate_panel_entrance(session_hero, 40);
  animate_panel_entrance(link_workflow_box, 140);
  animate_panel_entrance(runtime_overview, 220);
  apply_gui_auto_start();
  if (!gui_auto_start.auto_start && gui_auto_start.session_code.isEmpty()) {
    QTimer::singleShot(520, &window, [&]() {
      (void)generate_local_link_code();
    });
  }

#if defined(_DEBUG)
  const QString ui_snapshot_dir = qEnvironmentVariable("REDCLAW_UI_SNAPSHOT_DIR").trimmed();
  if (!ui_snapshot_dir.isEmpty() && !gui_auto_start.auto_start) {
    QTimer::singleShot(900, &window, [&]() {
      QDir snapshot_dir(ui_snapshot_dir);
      if (!snapshot_dir.mkpath(".")) {
        append_log(session_log, QString("UI snapshot directory could not be created: %1").arg(ui_snapshot_dir));
        QCoreApplication::exit(2);
        return;
      }

      auto save_snapshot = [&](const QString& name) {
        QApplication::processEvents();
        shell->layout()->activate();
        tabs->updateGeometry();
        session_log_box->updateGeometry();
        QApplication::processEvents();
        const QString path = snapshot_dir.filePath(name);
        if (!window.grab().save(path, "PNG")) {
          append_log(session_log, QString("UI snapshot could not be saved: %1").arg(path));
        }
      };

      connection_entry_page->set_local_code("AB12CD34");
      connection_entry_page->set_peer_code("EF56GH78");
      window.resize(900, 680);
      refresh_connection_flow_page();
      append_log(session_log, "UI snapshot check: the runtime log keeps multiple entries visible.");
      append_log(session_log, "UI snapshot check: the middle connection view switches independently.");
      append_log(session_log, "UI snapshot check: connection settings scroll without changing the network exit.");
      append_log(session_log, "UI snapshot check: the log remains available on every connection view.");
      save_snapshot("pre-900x680.png");

      connection_entry_page->peer_code_input()->setFocus(Qt::TabFocusReason);
      save_snapshot("pre-focus-900x680.png");
      connection_entry_page->set_actions_enabled(false);
      save_snapshot("pre-disabled-900x680.png");
      connection_entry_page->set_actions_enabled(true);

      connection_entry_page->network_settings_toggle()->setChecked(true);
      QApplication::processEvents();
      session_scroll->verticalScrollBar()->setValue(session_scroll->verticalScrollBar()->maximum());
      save_snapshot("pre-settings-900x680.png");
      connection_entry_page->network_settings_toggle()->setChecked(false);
      session_scroll->verticalScrollBar()->setValue(0);

      connection_flow_model.start(ConnectionFlowRole::kController);
      connecting_page->set_connection_code("EF56GH78");
      refresh_connection_flow_page();
      save_snapshot("connecting-900x680.png");
      apply_connection_flow_event(
          ConnectionFlowEvent::kFatalFailure,
          "No waiting device was found for that code. Confirm the other device is waiting, then retry.");
      save_snapshot("connecting-failed-900x680.png");

      connection_flow_model.start(ConnectionFlowRole::kHost);
      waiting_page->set_connection_code("AB12CD34");
      window.resize(800, 600);
      refresh_connection_flow_page();
      save_snapshot("waiting-800x600.png");
      QCoreApplication::quit();
    });
  }
#endif

  GuiQuitBarrier quit_barrier(app, controller.process(), diagnostic_writer,
      [&] { controller.request_stop(); },
      [&] {
        latency_heartbeat.stop(); latency_window.stop(); debug_status_timer->stop();
        debug_status.runtime_running = false; debug_status.runtime_pid = 0; debug_status.phase = "stopped";
        persist_debug_status(true);
      }, [process_logger](QString* error) {
        if (!process_logger) return true;
        process_logger->flush();
        const bool ok = process_logger->is_healthy();
        if (!ok && error) *error = "final diagnostic flush failed";
        return ok;
      });
  initialization_timing.finish();
  app.latency.begin_event_loop();
  const int app_result = app.exec();
#if defined(_WIN32) && !defined(NDEBUG)
  qa_input_probe.reset();
#endif
  remote_input_capture->shutdown();
  debug_control_server.close();
  g_diagnostic_writer = nullptr;

  if (error_detail != nullptr) {
    error_detail->clear();
  }

  return app_result == 0;
}

}  // namespace redclaw::ui

#else

namespace redclaw::ui {

bool launch_gui_shell(int, char**, redclaw::diag::ProcessFileLogger*, std::string* error_detail) {
  if (error_detail != nullptr) {
    *error_detail = "Qt GUI support is unavailable in this build.";
  }
  return false;
}

}  // namespace redclaw::ui

#endif
