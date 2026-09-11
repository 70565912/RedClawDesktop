#include "ui/agent_control_server.h"

#include <chrono>
#include <unordered_map>
#include <utility>
#include <deque>

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QUuid>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>

#include "redclaw/agent/agent_coordinator.h"
#include "redclaw/agent/agent_peer_session.h"

namespace redclaw::ui {
namespace {

std::uint64_t now_ms() {
  return static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
}

std::filesystem::path default_journal_path() {
  const QString root = QStandardPaths::writableLocation(
      QStandardPaths::AppLocalDataLocation);
  return std::filesystem::path(
      QDir(root).filePath("coordination/coordination-v1.jsonl").toStdWString());
}

bool outbound_request(redclaw::protocol::AgentMessageTypeV1 type) {
  return redclaw::agent::agent_message_route(type)
      == redclaw::agent::AgentMessageRoute::kLocalExecutor;
}

}  // namespace

class AgentControlServer::Impl final {
public:
  Impl(
      AgentControlServerConfig value,
      SendMessage send,
      LogMessage log,
      QObject* parent)
      : config(std::move(value)),
        send_message(std::move(send)),
        log_message(std::move(log)),
        server(parent),
        coordinator({
            .journal_path = config.journal_path.empty()
                ? default_journal_path() : config.journal_path,
            .git_sha = config.git_sha,
            .executable_sha256 = config.executable_sha256,
        }),
        local_epoch("agent-control-" + QUuid::createUuid()
            .toString(QUuid::WithoutBraces).toStdString()) {}

  bool start(QString* error_detail) {
    server.setSocketOptions(QLocalServer::UserAccessOption);
    const bool listening = !config.listen_external || server.listen(config.pipe_name);
    if (!listening) {
      assign_error(
          QString("Agent control listen failed name=%1 error=%2")
              .arg(config.pipe_name, server.errorString()),
          error_detail);
    }
    QObject::connect(&server, &QLocalServer::newConnection, &server, [this]() {
      accept_connections();
    });
    auto* timer = new QTimer(&server);
    timer->setInterval(50);
    QObject::connect(timer, &QTimer::timeout, &server, [this] { drain_results(); });
    timer->start();
    if (log_message) {
      log_message(QString("Agent control IPC ready; journal initializing asynchronously: name=%1 access=current-user authority=%2")
          .arg(config.pipe_name, authority_name()));
    }
    if (listening && error_detail != nullptr) {
      error_detail->clear();
    }
    return listening;
  }

  bool observe_remote(const redclaw::protocol::AgentMessageEnvelopeV1& message) {
    std::string error;
    // Caller retains one pending envelope and stops draining IPC on pressure.
    // In particular, a last terminal result must not disappear without a sync.
    return coordinator.enqueue(message, true, 0, &error);
  }

  QString authority_name() const {
    const auto name = redclaw::agent::to_string(coordinator.snapshot().authority);
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
  }

  void accept_connections() {
    while (QLocalSocket* socket = server.nextPendingConnection()) {
      if (server.findChildren<QLocalSocket*>().size() > 16) {
        socket->abort(); socket->deleteLater(); continue;
      }
      socket->setReadBufferSize(redclaw::protocol::kMaxLocalRuntimeAgentFrameBytes + 1);
      socket->setParent(&server);
      socket->setProperty("agent_request_buffer", QByteArray());
      socket->setProperty("agent_request_complete", false);
      QObject::connect(socket, &QLocalSocket::readyRead, socket, [this, socket]() {
        read_request(socket);
      });
      QObject::connect(socket, &QLocalSocket::disconnected,
                       socket, &QLocalSocket::deleteLater);
      read_request(socket);
    }
  }

  void read_request(QLocalSocket* socket) {
    if (socket == nullptr || socket->property("agent_request_complete").toBool()) {
      return;
    }
    QByteArray buffer = socket->property("agent_request_buffer").toByteArray();
    buffer.append(socket->readAll());
    if (buffer.size() > static_cast<qsizetype>(
            redclaw::protocol::kMaxLocalRuntimeAgentFrameBytes)) {
      socket->setProperty("agent_request_complete", true);
      write_error(socket, {}, "request_too_large",
                  "local Agent control frame exceeds 65536 bytes");
      return;
    }
    const qsizetype newline = buffer.indexOf('\n');
    if (newline < 0) {
      socket->setProperty("agent_request_buffer", buffer);
      return;
    }
    socket->setProperty("agent_request_complete", true);
    const QByteArray payload = buffer.left(newline).trimmed();
    const auto parsed = redclaw::protocol::parse_local_runtime_agent_frame_v1(
        payload.toStdString());
    if (!parsed.ok) {
      write_error(socket, {}, "invalid_agent_frame", parsed.error);
      return;
    }
    auto message = parsed.value;
    if (!outbound_request(message.type)) {
      write_error(socket, message, "operation_not_allowed",
                  "local Agent control accepts typed request operations only");
      return;
    }
    const auto epoch = QString::fromStdString(message.session_epoch);
    const auto last = local_message_ids.constFind(epoch);
    if (last != local_message_ids.cend() && message.message_id <= last.value()) {
      write_error(socket, message, "replay_or_reordering",
                  "local Agent control message ID did not increase");
      return;
    }
    if (last == local_message_ids.cend() && local_message_ids.size() >= 16) {
      write_error(socket, message, "client_epoch_limit",
                  "local Agent control client epoch limit reached");
      return;
    }
    local_message_ids[epoch] = message.message_id;

    std::string state_error;
    const auto cookie = ++next_cookie;
    if (!coordinator.enqueue(message, false, cookie, &state_error)) {
      write_error(socket, message, "coordination_rejected", state_error);
      return;
    }
    pending_requests.emplace(cookie, socket);
  }

  void drain_results() {
    // A journal fault disables Agent operations, not the desktop lifecycle.
    const auto health = coordinator.snapshot();
    if (!health.ready && !health.error.empty() && health.error != last_health_error) {
      if (log_message) log_message("Agent unavailable: " + QString::fromStdString(health.error));
      last_health_error = health.error;
    }
    QElapsedTimer budget;
    budget.start();
    while (budget.nsecsElapsed() < 2000000 && durable_messages.size() < 128) {
      auto results = pending_dispatch
          ? std::vector<redclaw::agent::AgentCoordinationResult>{std::move(*pending_dispatch)}
          : coordinator.take_results(1);
      pending_dispatch.reset();
      if (results.empty()) break;
      auto& result = results.front();
      ++processed_total;
      max_persistence_delay_ms = std::max(max_persistence_delay_ms,
          result.persisted_at_ms - result.enqueued_at_ms);
      last_persisted_at_ms = result.persisted_at_ms;
      QPointer<QLocalSocket> socket;
      if (result.cookie != 0) {
        const auto found = pending_requests.find(result.cookie);
        if (found != pending_requests.end()) {
          socket = found->second;
          pending_requests.erase(found);
        }
      }
      if (result.kind == redclaw::agent::AgentCoordinationResultKind::kRejected) {
        if (socket) write_error(socket, result.message, "coordination_rejected", result.error);
        else if (log_message) log_message("Agent coordination rejected: " + QString::fromStdString(result.error));
      } else if (result.kind == redclaw::agent::AgentCoordinationResultKind::kRemoteDurable) {
        durable_messages.push_back(std::move(result.message));
      } else if (result.kind == redclaw::agent::AgentCoordinationResultKind::kLocalSnapshot) {
        if (socket) write_dispatched(socket, result.message, true);
      } else {
        QString send_error;
        if (!send_message || !send_message(result.message, &send_error)) {
          if (socket) pending_requests[result.cookie] = socket;
          pending_dispatch = std::move(result);
          if (send_error != last_dispatch_error && log_message) log_message("Agent dispatch waiting: " + send_error);
          last_dispatch_error = send_error;
          break;
        } else if (socket) {
          write_dispatched(socket, result.message);
        }
      }
    }
    if (log_message && now_ms() - last_stats_at_ms >= 1000) {
      const auto stats = coordinator.snapshot();
      log_message(QString("Agent local pipeline ready=%1 queue=%2 processed=%3 rejected=%4 parsed_records=%5 persistence_delay_max_ms=%6 persisted_at_ms=%7 ui_dispatch_at_ms=%8")
          .arg(stats.ready).arg(stats.queue_depth).arg(processed_total).arg(stats.rejected_total)
          .arg(stats.parsed_records).arg(max_persistence_delay_ms).arg(last_persisted_at_ms).arg(now_ms()));
      last_stats_at_ms = now_ms();
    }
  }

  void write_dispatched(QLocalSocket* socket,
      const redclaw::protocol::AgentMessageEnvelopeV1& message, bool local_snapshot = false) {
    auto response = message;
    response.session_epoch = local_epoch;
    response.message_id = next_response_message_id++;
    response.sent_at_ms = now_ms();
    response.type = redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot;
    response.task_id = message.task_id;
    response.request_id = message.request_id;
    response.supersedes_request_id = message.supersedes_request_id;
    response.evidence_manifest_name = message.evidence_manifest_name;
    response.evidence_sha256 = message.evidence_sha256;
    response.complete = true;
    response.event_kind = local_snapshot ? "local_snapshot" : "local_dispatched";
    if (response.task_state == redclaw::protocol::AgentTaskStateV1::kUnavailable) {
      response.task_state = redclaw::protocol::AgentTaskStateV1::kQueued;
    }
    write_response(socket, response);
  }

  void write_error(
      QLocalSocket* socket,
      const redclaw::protocol::AgentMessageEnvelopeV1& request,
      std::string error_code,
      std::string error_text) {
    redclaw::protocol::AgentMessageEnvelopeV1 response;
    response.session_epoch = local_epoch;
    response.message_id = next_response_message_id++;
    response.sent_at_ms = now_ms();
    response.type = redclaw::protocol::AgentMessageTypeV1::kTaskError;
    response.task_id = request.task_id;
    response.request_id = request.request_id;
    response.supersedes_request_id = request.supersedes_request_id;
    response.task_state = redclaw::protocol::AgentTaskStateV1::kFailed;
    response.error_code = std::move(error_code);
    if (error_text.size() > redclaw::protocol::kMaxAgentEventChunkBytes) {
      error_text.resize(redclaw::protocol::kMaxAgentEventChunkBytes);
    }
    response.text = std::move(error_text);
    response.complete = true;
    write_response(socket, response);
  }

  void write_response(
      QLocalSocket* socket,
      const redclaw::protocol::AgentMessageEnvelopeV1& response) {
    const std::string encoded =
        redclaw::protocol::serialize_local_runtime_agent_frame_v1(response);
    QByteArray wire(encoded.data(), static_cast<qsizetype>(encoded.size()));
    wire.push_back('\n');
    socket->write(wire);
    socket->flush();
    socket->disconnectFromServer();
  }

  static void assign_error(const QString& value, QString* error_detail) {
    if (error_detail != nullptr) {
      *error_detail = value;
    }
  }

  AgentControlServerConfig config;
  SendMessage send_message;
  LogMessage log_message;
  QLocalServer server;
  redclaw::agent::AgentCoordinator coordinator;
  std::deque<redclaw::protocol::AgentMessageEnvelopeV1> durable_messages;
  std::unordered_map<std::uint64_t, QPointer<QLocalSocket>> pending_requests;
  std::optional<redclaw::agent::AgentCoordinationResult> pending_dispatch;
  QString last_dispatch_error;
  std::uint64_t next_cookie = 0;
  std::uint64_t processed_total = 0;
  std::uint64_t max_persistence_delay_ms = 0;
  std::uint64_t last_persisted_at_ms = 0;
  std::uint64_t last_stats_at_ms = 0;
  QHash<QString, std::uint64_t> local_message_ids;
  std::string local_epoch;
  std::string last_health_error;
  std::uint64_t next_response_message_id = 1;
};

redclaw::protocol::AgentMessageEnvelopeV1 make_gui_agent_command(
    redclaw::protocol::AgentMessageTypeV1 type) {
  redclaw::protocol::AgentMessageEnvelopeV1 message;
  message.type = type;
  message.request_id = "request-" + QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
  return message;
}

AgentControlServer::AgentControlServer(
    AgentControlServerConfig config,
    SendMessage send_message,
    LogMessage log_message,
    QObject* parent)
    : QObject(parent),
      impl_(std::make_unique<Impl>(
          std::move(config), std::move(send_message), std::move(log_message), this)) {}

AgentControlServer::~AgentControlServer() = default;

bool AgentControlServer::start(QString* error_detail) {
  return impl_->start(error_detail);
}

bool AgentControlServer::observe_remote(
    const redclaw::protocol::AgentMessageEnvelopeV1& message) {
  return impl_->observe_remote(message);
}

QString AgentControlServer::pipe_name() const {
  return impl_->config.pipe_name;
}

QString AgentControlServer::authority_name() const {
  return impl_->authority_name();
}

bool AgentControlServer::sync_required() const {
  return impl_->coordinator.snapshot().sync_required;
}

bool AgentControlServer::available() const {
  return impl_->coordinator.snapshot().ready;
}

void AgentControlServer::request_sync(std::string task_id) {
  impl_->coordinator.request_sync(std::move(task_id));
}

bool AgentControlServer::submit(redclaw::protocol::AgentMessageEnvelopeV1 message, QString* error_detail) {
  std::string error;
  const bool queued = impl_->coordinator.enqueue(std::move(message), false, 0, &error);
  if (error_detail) *error_detail = QString::fromStdString(error);
  return queued;
}

std::vector<redclaw::protocol::AgentMessageEnvelopeV1> AgentControlServer::take_durable_messages() {
  std::vector<redclaw::protocol::AgentMessageEnvelopeV1> result;
  while (!impl_->durable_messages.empty() && result.size() < 1) {
    result.push_back(std::move(impl_->durable_messages.front()));
    impl_->durable_messages.pop_front();
  }
  return result;
}

}  // namespace redclaw::ui
