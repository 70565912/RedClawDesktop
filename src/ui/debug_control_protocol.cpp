#include "ui/debug_control_protocol.h"

#include <algorithm>
#include <utility>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

namespace redclaw::ui {

namespace {

bool is_json_integer_in_range(const QJsonValue& value, int minimum, int maximum) {
  if (!value.isDouble()) {
    return false;
  }
  const double number = value.toDouble();
  const int integer = value.toInt(minimum - 1);
  return number == static_cast<double>(integer) && integer >= minimum && integer <= maximum;
}

DebugControlParseResult failure(
    const QString& code,
    const QString& detail,
    const QString& request_id = "unknown",
    const QString& action_name = {}) {
  DebugControlParseResult result;
  result.request.request_id = request_id;
  result.action_name = action_name;
  result.error_code = code;
  result.error_detail = detail;
  return result;
}

}  // namespace

DebugControlParseResult parse_debug_control_request(const QByteArray& payload) {
  QJsonParseError parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(payload, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    return failure("invalid_json", parse_error.errorString());
  }

  const QJsonObject object = document.object();
  const QJsonValue request_id_value = object.value("request_id");
  const QString request_id = request_id_value.isString()
      ? request_id_value.toString()
      : QString();
  const QString action_name = object.value("action").toString();
  if (object.value("schema").toString() != "redclaw.debug-control.v1") {
    return failure("invalid_schema", "schema must be redclaw.debug-control.v1", request_id, action_name);
  }
  if (!request_id_value.isString() || request_id.isEmpty() || request_id.size() > 64) {
    return failure(
        "invalid_request_id",
        "request_id must contain 1..64 characters",
        request_id.isEmpty() ? "unknown" : request_id,
        action_name);
  }

  QSet<QString> allowed_fields = {"schema", "request_id", "action"};
  if (action_name == "start") {
    allowed_fields.insert("role");
  } else if (action_name == "tail_log" || action_name == "remote_log_read") {
    allowed_fields.insert("limit");
  } else if (action_name == "remote_input_mouse_click") {
    allowed_fields.insert("x");
    allowed_fields.insert("y");
    allowed_fields.insert("button");
  } else if (action_name == "remote_input_key_press") {
    allowed_fields.insert("scan_code");
    allowed_fields.insert("virtual_key");
    allowed_fields.insert("extended");
  } else if (action_name == "agent_start_fixture") {
    allowed_fields.insert("fixture_id");
    allowed_fields.insert("provider");
  } else if (action_name == "agent_approval") {
    allowed_fields.insert("decision");
  }
  for (const QString& key : object.keys()) {
    if (!allowed_fields.contains(key)) {
      return failure("unsupported_field", QString("field is not accepted: %1").arg(key), request_id, action_name);
    }
  }

  DebugControlParseResult result;
  result.request.request_id = request_id;
  result.action_name = action_name;
  if (action_name == "status") {
    result.request.action = DebugControlAction::kStatus;
  } else if (action_name == "measurement_arm") {
    result.request.action = DebugControlAction::kMeasurementArm;
  } else if (action_name == "measurement_export") {
    result.request.action = DebugControlAction::kMeasurementExport;
  } else if (action_name == "input_probe_start") {
    result.request.action = DebugControlAction::kInputProbeStart;
  } else if (action_name == "input_probe_stop") {
    result.request.action = DebugControlAction::kInputProbeStop;
  } else if (action_name == "input_probe_export") {
    result.request.action = DebugControlAction::kInputProbeExport;
  } else if (action_name == "start") {
    result.request.action = DebugControlAction::kStart;
    result.request.role = object.value("role").toString().trimmed().toLower();
    if (result.request.role != "host" && result.request.role != "controller") {
      return failure("invalid_role", "role must be host or controller", request_id, action_name);
    }
  } else if (action_name == "reconnect") {
    result.request.action = DebugControlAction::kReconnect;
  } else if (action_name == "stop") {
    result.request.action = DebugControlAction::kStop;
  } else if (action_name == "exit") {
    result.request.action = DebugControlAction::kExit;
  } else if (action_name == "tail_log") {
    result.request.action = DebugControlAction::kTailLog;
    result.request.tail_limit = object.value("limit").toInt(100);
    if (result.request.tail_limit < 1 || result.request.tail_limit > 200) {
      return failure("invalid_limit", "limit must be within 1..200", request_id, action_name);
    }
  } else if (action_name == "remote_log_snapshot") {
    result.request.action = DebugControlAction::kRemoteLogSnapshot;
  } else if (action_name == "remote_log_read") {
    result.request.action = DebugControlAction::kRemoteLogRead;
    result.request.tail_limit = object.value("limit").toInt(100);
    if (result.request.tail_limit < 1 || result.request.tail_limit > 200) {
      return failure("invalid_limit", "limit must be within 1..200", request_id, action_name);
    }
  } else if (action_name == "remote_control_start") {
    result.request.action = DebugControlAction::kRemoteControlStart;
  } else if (action_name == "remote_control_pause") {
    result.request.action = DebugControlAction::kRemoteControlPause;
  } else if (action_name == "remote_input_mouse_click") {
    result.request.action = DebugControlAction::kRemoteInputMouseClick;
    const QJsonValue x = object.value("x");
    const QJsonValue y = object.value("y");
    const QString button = object.value("button").toString("left").trimmed().toLower();
    if (!is_json_integer_in_range(x, 0, 65535)
        || !is_json_integer_in_range(y, 0, 65535)) {
      return failure(
          "invalid_coordinates",
          "x and y must be integer normalized coordinates within 0..65535",
          request_id,
          action_name);
    }
    if (!QSet<QString>{"left", "right", "middle", "x1", "x2"}.contains(button)) {
      return failure(
          "invalid_mouse_button",
          "button must be left, right, middle, x1, or x2",
          request_id,
          action_name);
    }
    result.request.normalized_x = static_cast<std::uint16_t>(x.toInt());
    result.request.normalized_y = static_cast<std::uint16_t>(y.toInt());
    result.request.mouse_button = button;
  } else if (action_name == "remote_input_key_press") {
    result.request.action = DebugControlAction::kRemoteInputKeyPress;
    const QJsonValue scan_code = object.value("scan_code");
    const QJsonValue virtual_key = object.value("virtual_key");
    const QJsonValue extended = object.value("extended");
    if (!is_json_integer_in_range(scan_code, 1, 255)
        || !is_json_integer_in_range(virtual_key, 0, 255)) {
      return failure(
          "invalid_key",
          "scan_code must be within 1..255 and virtual_key within 0..255",
          request_id,
          action_name);
    }
    if (!extended.isUndefined() && !extended.isBool()) {
      return failure(
          "invalid_extended_flag",
          "extended must be a boolean",
          request_id,
          action_name);
    }
    result.request.scan_code = static_cast<std::uint16_t>(scan_code.toInt());
    result.request.virtual_key = static_cast<std::uint16_t>(virtual_key.toInt());
    result.request.extended = extended.toBool(false);
  } else if (action_name == "agent_status") {
    result.request.action = DebugControlAction::kAgentStatus;
  } else if (action_name == "agent_start_fixture") {
    result.request.action = DebugControlAction::kAgentStartFixture;
    result.request.fixture_id = object.value("fixture_id").toString().trimmed();
    result.request.agent_provider = object.value("provider").toString("codex").trimmed().toLower();
    if (!QSet<QString>{"inspect_project", "write_marker_and_test"}.contains(
            result.request.fixture_id)) {
      return failure(
          "invalid_fixture",
          "fixture_id must be inspect_project or write_marker_and_test",
          request_id,
          action_name);
    }
    if (!QSet<QString>{"codex", "cursor"}.contains(result.request.agent_provider)) {
      return failure(
          "invalid_agent_provider",
          "provider must be codex or cursor",
          request_id,
          action_name);
    }
  } else if (action_name == "agent_interrupt") {
    result.request.action = DebugControlAction::kAgentInterrupt;
  } else if (action_name == "agent_follow_up_fixture") {
    result.request.action = DebugControlAction::kAgentFollowUpFixture;
  } else if (action_name == "agent_approval") {
    result.request.action = DebugControlAction::kAgentApproval;
    result.request.approval_decision = object.value("decision").toString().trimmed().toLower();
    if (result.request.approval_decision != "accept"
        && result.request.approval_decision != "reject") {
      return failure(
          "invalid_decision",
          "decision must be accept or reject",
          request_id,
          action_name);
    }
  } else if (action_name == "agent_evidence") {
    result.request.action = DebugControlAction::kAgentEvidence;
  } else if (action_name == "export_evidence") {
    result.request.action = DebugControlAction::kExportEvidence;
  } else {
    return failure("unknown_action", "action is not supported", request_id, action_name);
  }

  result.ok = true;
  result.error_code = "none";
  result.error_detail.clear();
  return result;
}

QString debug_control_action_name(DebugControlAction action) {
  switch (action) {
  case DebugControlAction::kStatus:
    return "status";
  case DebugControlAction::kStart:
    return "start";
  case DebugControlAction::kReconnect:
    return "reconnect";
  case DebugControlAction::kStop:
    return "stop";
  case DebugControlAction::kExit:
    return "exit";
  case DebugControlAction::kTailLog:
    return "tail_log";
  case DebugControlAction::kRemoteLogSnapshot:
    return "remote_log_snapshot";
  case DebugControlAction::kRemoteLogRead:
    return "remote_log_read";
  case DebugControlAction::kRemoteControlStart:
    return "remote_control_start";
  case DebugControlAction::kRemoteControlPause:
    return "remote_control_pause";
  case DebugControlAction::kRemoteInputMouseClick:
    return "remote_input_mouse_click";
  case DebugControlAction::kRemoteInputKeyPress:
    return "remote_input_key_press";
  case DebugControlAction::kAgentStatus:
    return "agent_status";
  case DebugControlAction::kMeasurementArm:
    return "measurement_arm";
  case DebugControlAction::kMeasurementExport:
    return "measurement_export";
  case DebugControlAction::kInputProbeStart: return "input_probe_start";
  case DebugControlAction::kInputProbeStop: return "input_probe_stop";
  case DebugControlAction::kInputProbeExport: return "input_probe_export";
  case DebugControlAction::kAgentStartFixture:
    return "agent_start_fixture";
  case DebugControlAction::kAgentFollowUpFixture:
    return "agent_follow_up_fixture";
  case DebugControlAction::kAgentInterrupt:
    return "agent_interrupt";
  case DebugControlAction::kAgentApproval:
    return "agent_approval";
  case DebugControlAction::kAgentEvidence:
    return "agent_evidence";
  case DebugControlAction::kExportEvidence:
    return "export_evidence";
  }
  return "unknown";
}

QStringList bounded_tail_log_lines(
    const QStringList& lines,
    int limit,
    qsizetype max_line_chars,
    qsizetype max_response_bytes) {
  QStringList result;
  if (limit < 1 || max_line_chars < 1 || max_response_bytes < 1) {
    return result;
  }
  qsizetype response_bytes = 0;
  const qsizetype begin = (std::max)(qsizetype(0), lines.size() - limit);
  for (qsizetype index = lines.size(); index > begin; --index) {
    QString line = lines.at(index - 1);
    if (line.size() > max_line_chars) {
      line = line.left(max_line_chars);
    }
    const qsizetype line_bytes = line.toUtf8().size();
    if (response_bytes + line_bytes > max_response_bytes) {
      break;
    }
    response_bytes += line_bytes;
    result.push_front(std::move(line));
  }
  return result;
}

void DebugRemoteLogSnapshot::begin(const QString& request_id) {
  request_id_ = request_id;
  error_detail_.clear();
  lines_.clear();
  complete_ = false;
}

void DebugRemoteLogSnapshot::append_chunk(
    const QString& request_id,
    const QString& payload,
    bool gap) {
  if (request_id_.isEmpty() || request_id != request_id_ || complete_) {
    return;
  }
  if (gap) {
    lines_.push_back("--- remote log gap: older lines were evicted ---");
  }
  lines_.append(payload.split('\n', Qt::SkipEmptyParts));
  constexpr qsizetype kMaxSnapshotLines = 50;
  while (lines_.size() > kMaxSnapshotLines) {
    lines_.removeFirst();
  }
}

void DebugRemoteLogSnapshot::complete(const QString& request_id) {
  if (!request_id_.isEmpty() && request_id == request_id_) {
    complete_ = true;
  }
}

void DebugRemoteLogSnapshot::fail(const QString& request_id, const QString& error_detail) {
  if (!request_id_.isEmpty() && request_id == request_id_) {
    error_detail_ = error_detail;
    complete_ = true;
  }
}

bool DebugRemoteLogSnapshot::active() const {
  return !request_id_.isEmpty();
}

bool DebugRemoteLogSnapshot::is_complete() const {
  return complete_;
}

QString DebugRemoteLogSnapshot::request_id() const {
  return request_id_;
}

QString DebugRemoteLogSnapshot::error_detail() const {
  return error_detail_;
}

QStringList DebugRemoteLogSnapshot::tail_lines(int limit) const {
  return bounded_tail_log_lines(lines_, limit);
}

}  // namespace redclaw::ui
