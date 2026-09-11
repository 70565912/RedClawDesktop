#pragma once

#include <cstdint>

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace redclaw::ui {

enum class DebugControlAction {
  kStatus,
  kStart,
  kReconnect,
  kStop,
  kExit,
  kTailLog,
  kRemoteLogSnapshot,
  kRemoteLogRead,
  kRemoteControlStart,
  kRemoteControlPause,
  kRemoteInputMouseClick,
  kRemoteInputKeyPress,
  kAgentStatus,
  kMeasurementArm,
  kMeasurementExport,
  kInputProbeStart,
  kInputProbeStop,
  kInputProbeExport,
  kAgentStartFixture,
  kAgentFollowUpFixture,
  kAgentInterrupt,
  kAgentApproval,
  kAgentEvidence,
  kExportEvidence,
};

struct DebugControlRequest {
  QString request_id;
  DebugControlAction action = DebugControlAction::kStatus;
  QString role;
  int tail_limit = 100;
  std::uint16_t normalized_x = 32768;
  std::uint16_t normalized_y = 32768;
  QString mouse_button = "left";
  std::uint16_t scan_code = 0;
  std::uint16_t virtual_key = 0;
  bool extended = false;
  QString fixture_id;
  QString agent_provider;
  QString approval_decision;
};

struct DebugControlParseResult {
  bool ok = false;
  DebugControlRequest request;
  QString action_name;
  QString error_code;
  QString error_detail;
};

[[nodiscard]] DebugControlParseResult parse_debug_control_request(const QByteArray& payload);

[[nodiscard]] QString debug_control_action_name(DebugControlAction action);

[[nodiscard]] QStringList bounded_tail_log_lines(
    const QStringList& lines,
    int limit,
    qsizetype max_line_chars = 1024,
    qsizetype max_response_bytes = 56 * 1024);

class DebugRemoteLogSnapshot {
public:
  void begin(const QString& request_id);
  void append_chunk(const QString& request_id, const QString& payload, bool gap);
  void complete(const QString& request_id);
  void fail(const QString& request_id, const QString& error_detail);

  [[nodiscard]] bool active() const;
  [[nodiscard]] bool is_complete() const;
  [[nodiscard]] QString request_id() const;
  [[nodiscard]] QString error_detail() const;
  [[nodiscard]] QStringList tail_lines(int limit) const;

private:
  QString request_id_;
  QString error_detail_;
  QStringList lines_;
  bool complete_ = false;
};

}  // namespace redclaw::ui
