#include <gtest/gtest.h>

#include "ui/debug_control_protocol.h"

namespace {

TEST(DebugControlProtocol, ParsesSupportedRequests) {
  const auto start = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-1","action":"start","role":"host"})");
  ASSERT_TRUE(start.ok) << start.error_detail.toStdString();
  EXPECT_EQ(start.request.action, redclaw::ui::DebugControlAction::kStart);
  EXPECT_EQ(start.request.role, "host");

  const auto tail = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-2","action":"tail_log","limit":200})");
  ASSERT_TRUE(tail.ok) << tail.error_detail.toStdString();
  EXPECT_EQ(tail.request.tail_limit, 200);

  const auto remote_snapshot = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-3","action":"remote_log_snapshot"})");
  ASSERT_TRUE(remote_snapshot.ok) << remote_snapshot.error_detail.toStdString();
  EXPECT_EQ(remote_snapshot.request.action, redclaw::ui::DebugControlAction::kRemoteLogSnapshot);

  const auto remote_read = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-4","action":"remote_log_read","limit":80})");
  ASSERT_TRUE(remote_read.ok) << remote_read.error_detail.toStdString();
  EXPECT_EQ(remote_read.request.action, redclaw::ui::DebugControlAction::kRemoteLogRead);
  EXPECT_EQ(remote_read.request.tail_limit, 80);

  const auto control_start = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-5","action":"remote_control_start"})");
  ASSERT_TRUE(control_start.ok) << control_start.error_detail.toStdString();
  EXPECT_EQ(
      control_start.request.action,
      redclaw::ui::DebugControlAction::kRemoteControlStart);

  const auto mouse_click = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-6","action":"remote_input_mouse_click","x":32768,"y":16384,"button":"right"})");
  ASSERT_TRUE(mouse_click.ok) << mouse_click.error_detail.toStdString();
  EXPECT_EQ(
      mouse_click.request.action,
      redclaw::ui::DebugControlAction::kRemoteInputMouseClick);
  EXPECT_EQ(mouse_click.request.normalized_x, 32768);
  EXPECT_EQ(mouse_click.request.normalized_y, 16384);
  EXPECT_EQ(mouse_click.request.mouse_button, "right");

  const auto key_press = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-7","action":"remote_input_key_press","scan_code":30,"virtual_key":65,"extended":false})");
  ASSERT_TRUE(key_press.ok) << key_press.error_detail.toStdString();
  EXPECT_EQ(
      key_press.request.action,
      redclaw::ui::DebugControlAction::kRemoteInputKeyPress);
  EXPECT_EQ(key_press.request.scan_code, 30);
  EXPECT_EQ(key_press.request.virtual_key, 65);
  EXPECT_FALSE(key_press.request.extended);

  const auto agent_fixture = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-8","action":"agent_start_fixture","fixture_id":"inspect_project","provider":"cursor"})");
  ASSERT_TRUE(agent_fixture.ok) << agent_fixture.error_detail.toStdString();
  EXPECT_EQ(agent_fixture.request.action,
            redclaw::ui::DebugControlAction::kAgentStartFixture);
  EXPECT_EQ(agent_fixture.request.fixture_id, "inspect_project");
  EXPECT_EQ(agent_fixture.request.agent_provider, "cursor");

  const auto agent_follow_up = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-8b","action":"agent_follow_up_fixture"})");
  ASSERT_TRUE(agent_follow_up.ok) << agent_follow_up.error_detail.toStdString();
  EXPECT_EQ(agent_follow_up.request.action,
            redclaw::ui::DebugControlAction::kAgentFollowUpFixture);

  const auto agent_approval = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-9","action":"agent_approval","decision":"reject"})");
  ASSERT_TRUE(agent_approval.ok) << agent_approval.error_detail.toStdString();
  EXPECT_EQ(agent_approval.request.action,
            redclaw::ui::DebugControlAction::kAgentApproval);
  EXPECT_EQ(agent_approval.request.approval_decision, "reject");
}

TEST(DebugControlProtocol, TracksOneBoundedRemoteLogSnapshot) {
  redclaw::ui::DebugRemoteLogSnapshot snapshot;
  snapshot.begin("snapshot-1");
  snapshot.append_chunk("stale-request", "ignored", false);
  snapshot.append_chunk("snapshot-1", "line-1\nline-2", true);

  EXPECT_TRUE(snapshot.active());
  EXPECT_FALSE(snapshot.is_complete());
  EXPECT_EQ(snapshot.tail_lines(10).size(), 3);

  snapshot.complete("snapshot-1");
  EXPECT_TRUE(snapshot.is_complete());
  EXPECT_TRUE(snapshot.error_detail().isEmpty());

  snapshot.begin("snapshot-2");
  snapshot.fail("snapshot-2", "remote diagnostics are disabled");
  EXPECT_TRUE(snapshot.is_complete());
  EXPECT_EQ(snapshot.error_detail(), "remote diagnostics are disabled");
  EXPECT_TRUE(snapshot.tail_lines(10).isEmpty());
}

TEST(DebugControlProtocol, ReturnsStableValidationErrors) {
  const auto unsupported = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-1","action":"status","command":"whoami"})");
  EXPECT_FALSE(unsupported.ok);
  EXPECT_EQ(unsupported.error_code, "unsupported_field");

  const auto unknown = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-2","action":"shell"})");
  EXPECT_FALSE(unknown.ok);
  EXPECT_EQ(unknown.error_code, "unknown_action");

  const auto invalid_role = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-3","action":"start","role":"peer"})");
  EXPECT_FALSE(invalid_role.ok);
  EXPECT_EQ(invalid_role.error_code, "invalid_role");

  const auto invalid_limit = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-4","action":"tail_log","limit":201})");
  EXPECT_FALSE(invalid_limit.ok);
  EXPECT_EQ(invalid_limit.error_code, "invalid_limit");

  const auto missing_request_id = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","action":"status"})");
  EXPECT_FALSE(missing_request_id.ok);
  EXPECT_EQ(missing_request_id.error_code, "invalid_request_id");

  const auto non_string_request_id = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":123,"action":"status"})");
  EXPECT_FALSE(non_string_request_id.ok);
  EXPECT_EQ(non_string_request_id.error_code, "invalid_request_id");

  const auto invalid_coordinates = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-5","action":"remote_input_mouse_click","x":65536,"y":0,"button":"left"})");
  EXPECT_FALSE(invalid_coordinates.ok);
  EXPECT_EQ(invalid_coordinates.error_code, "invalid_coordinates");

  const auto invalid_key = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-6","action":"remote_input_key_press","scan_code":0,"virtual_key":65})");
  EXPECT_FALSE(invalid_key.ok);
  EXPECT_EQ(invalid_key.error_code, "invalid_key");

  const auto arbitrary_agent_instruction = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-7","action":"agent_start_fixture","fixture_id":"inspect_project","instruction":"delete files"})");
  EXPECT_FALSE(arbitrary_agent_instruction.ok);
  EXPECT_EQ(arbitrary_agent_instruction.error_code, "unsupported_field");

  const auto invalid_agent_fixture = redclaw::ui::parse_debug_control_request(
      R"({"schema":"redclaw.debug-control.v1","request_id":"req-8","action":"agent_start_fixture","fixture_id":"shell"})");
  EXPECT_FALSE(invalid_agent_fixture.ok);
  EXPECT_EQ(invalid_agent_fixture.error_code, "invalid_fixture");
}

TEST(DebugControlProtocol, BoundsTailResponseLinesAndBytes) {
  QStringList lines;
  for (int index = 0; index < 250; ++index) {
    lines.push_back(QString::number(index) + ":" + QString(2048, QChar('x')));
  }
  const QStringList bounded = redclaw::ui::bounded_tail_log_lines(lines, 200);
  EXPECT_LE(bounded.size(), 200);
  qsizetype bytes = 0;
  for (const QString& line : bounded) {
    EXPECT_LE(line.size(), 1024);
    bytes += line.toUtf8().size();
  }
  EXPECT_LE(bytes, 56 * 1024);
  ASSERT_FALSE(bounded.isEmpty());
  EXPECT_TRUE(bounded.last().startsWith("249:"));
}

}  // namespace
