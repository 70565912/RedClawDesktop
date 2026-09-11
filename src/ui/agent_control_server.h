#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include <QObject>
#include <QString>

#include "redclaw/protocol/agent_protocol.h"

namespace redclaw::ui {

// A new local GUI intent needs its own replay/dedup identity, including Stop.
// Approval decisions replace this ID with the peer's pending approval ID.
[[nodiscard]] redclaw::protocol::AgentMessageEnvelopeV1 make_gui_agent_command(
    redclaw::protocol::AgentMessageTypeV1 type);

struct AgentControlServerConfig {
  bool listen_external = true;
  QString pipe_name = "RedClawDesktop.AgentControl.v1";
  std::filesystem::path journal_path;
  std::string git_sha;
  std::string executable_sha256;
};

class AgentControlServer final : public QObject {
public:
  using SendMessage = std::function<bool(
      redclaw::protocol::AgentMessageEnvelopeV1,
      QString*)>;
  using LogMessage = std::function<void(const QString&)>;

  AgentControlServer(
      AgentControlServerConfig config,
      SendMessage send_message,
      LogMessage log_message,
      QObject* parent = nullptr);
  ~AgentControlServer() override;

  AgentControlServer(const AgentControlServer&) = delete;
  AgentControlServer& operator=(const AgentControlServer&) = delete;

  [[nodiscard]] bool start(QString* error_detail = nullptr);
  bool observe_remote(const redclaw::protocol::AgentMessageEnvelopeV1& message);
  [[nodiscard]] bool submit(redclaw::protocol::AgentMessageEnvelopeV1 message,
                            QString* error_detail = nullptr);
  [[nodiscard]] std::vector<redclaw::protocol::AgentMessageEnvelopeV1> take_durable_messages();

  [[nodiscard]] QString pipe_name() const;
  [[nodiscard]] QString authority_name() const;
  [[nodiscard]] bool sync_required() const;
  [[nodiscard]] bool available() const;
  void request_sync(std::string task_id = {});

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::ui
