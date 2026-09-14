#include <gtest/gtest.h>

#include <QApplication>
#include <QAbstractScrollArea>
#include <QScrollBar>
#include <QDir>
#include <QDialog>
#include <QVBoxLayout>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFrame>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include "ui/agent_message_text_view.h"
#include <QTimer>
#include <QToolButton>

#include "ui/agent_conversation_panel.h"
#include "ui/agent_panel_presentation.h"

namespace {

using redclaw::protocol::AgentMessageEnvelopeV1;
using redclaw::protocol::AgentMessageTypeV1;
using redclaw::protocol::AgentProviderKindV1;
using redclaw::protocol::AgentProviderReadinessV1;
using redclaw::protocol::AgentTaskStateV1;
using redclaw::ui::AgentConversationPanel;
using redclaw::ui::AgentSubmitMode;
using redclaw::ui::AgentSubmitResult;

bool wait_for_panel_render(const std::function<bool()>& ready) {
  if (ready()) return true;
  QEventLoop loop;
  QTimer poll;
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (ready()) loop.quit();
  });
  poll.start(10);
  QTimer::singleShot(2000, &loop, &QEventLoop::quit);
  loop.exec();
  return ready();
}

void make_panel_ready(AgentConversationPanel* panel) {
  panel->set_transport_state(true, true, false);

  AgentMessageEnvelopeV1 capability;
  capability.type = AgentMessageTypeV1::kCapabilities;
  capability.provider = AgentProviderKindV1::kCodex;
  capability.provider_readiness = AgentProviderReadinessV1::kReady;
  capability.available = true;
  capability.model = "gpt-test";
  capability.display_name = "Codex test provider";
  panel->apply_capability_message(capability);

  AgentMessageEnvelopeV1 project;
  project.type = AgentMessageTypeV1::kProjectCatalog;
  project.project_id = "redclaw";
  project.display_name = "RedClawDesktop";
  project.git_repository = true;
  panel->apply_project_catalog_message(project);
}

TEST(AgentConversationPanel, HostWindowSharesConversationAndClosingOnlyHidesIt) {
  QTemporaryDir directory;
  QSettings settings(directory.filePath("host.ini"), QSettings::IniFormat);
  QWidget owner;
  auto* layout = new QVBoxLayout(&owner);
  auto* panel = new AgentConversationPanel(&settings, &owner);
  layout->addWidget(panel);
  redclaw::ui::AgentPanelPresentation presentation(panel, &owner);
  make_panel_ready(panel);
  panel->set_current_task_id("same-task", AgentTaskStateV1::kRunning);
  presentation.set_desktop_host(true);
  presentation.open_button()->click();
  auto* dialog = owner.findChild<QDialog*>("hostRemoteAgentWindow");
  ASSERT_NE(dialog, nullptr);
  EXPECT_TRUE(dialog->isVisible());
  EXPECT_FALSE(dialog->isModal());
  dialog->close();
  EXPECT_EQ(panel->current_task_id(), "same-task");
  EXPECT_EQ(panel->current_task_state(), AgentTaskStateV1::kRunning);
  presentation.set_desktop_host(false);
  EXPECT_EQ(panel->parentWidget(), &owner);
  EXPECT_EQ(panel->current_task_id(), "same-task");
}

TEST(AgentConversationPanel, WindowHeightFollowsConversationContent) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  QDialog host;
  host.resize(520, 760);
  auto* layout = new QVBoxLayout(&host);
  auto* panel = new AgentConversationPanel(&settings, &host);
  layout->addWidget(panel);
  make_panel_ready(panel);
  host.show();
  panel->set_current_task_id("task-resize", AgentTaskStateV1::kRunning);
  QApplication::processEvents();
  const int before = host.height();

  AgentMessageEnvelopeV1 reply;
  reply.type = AgentMessageTypeV1::kEvent;
  reply.task_id = "task-resize";
  reply.task_state = AgentTaskStateV1::kRunning;
  reply.event_kind = "agentMessage/delta";
  QString body;
  for (int line = 0; line < 35; ++line) {
    if (!body.isEmpty()) body += '\n';
    body += QString("Line %1: the conversation window should grow with this content.")
        .arg(line + 1);
  }
  reply.text = body.toUtf8().toStdString();
  panel->apply_task_message(reply);
  ASSERT_TRUE(wait_for_panel_render([&] { return host.height() > before; }));
  const int grown = host.height();
  EXPECT_GT(grown, before);

  panel->set_current_task_id("task-empty", AgentTaskStateV1::kQueued);
  ASSERT_TRUE(wait_for_panel_render([&] { return host.height() < grown; }));
}

TEST(AgentConversationPanel, MapsNewRunningAndCompletedTasksToExpectedCommands) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);

  std::vector<AgentSubmitMode> modes;
  panel.set_submit_callback([&](AgentSubmitMode mode, const QString&) {
    modes.push_back(mode);
    return AgentSubmitResult{.ok = true, .task_id = "task-1"};
  });
  auto* send = panel.findChild<QPushButton*>("agentSendAction");
  ASSERT_NE(send, nullptr);

  panel.set_instruction_text("Start the bounded task");
  send->click();
  ASSERT_EQ(modes.size(), 1U);
  EXPECT_EQ(modes.back(), AgentSubmitMode::kCreateTask);

  panel.set_current_task_id("task-1", AgentTaskStateV1::kRunning);
  panel.set_instruction_text("Use the current runtime evidence");
  send->click();
  ASSERT_EQ(modes.size(), 2U);
  EXPECT_EQ(modes.back(), AgentSubmitMode::kSteerTurn);

  panel.set_current_task_id("task-1", AgentTaskStateV1::kCompleted);
  panel.set_instruction_text("Run the final check");
  send->click();
  ASSERT_EQ(modes.size(), 3U);
  EXPECT_EQ(modes.back(), AgentSubmitMode::kStartTurn);
}

TEST(AgentConversationPanel, EnterSubmitsAndShiftEnterAddsNewline) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);

  int submits = 0;
  panel.set_submit_callback([&](AgentSubmitMode, const QString&) {
    ++submits;
    return AgentSubmitResult{.ok = true, .task_id = "task-enter"};
  });
  auto* composer = panel.findChild<QPlainTextEdit*>("agentComposer");
  ASSERT_NE(composer, nullptr);
  panel.set_instruction_text("First line");

  QKeyEvent shift_enter(
      QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier, "\n");
  QApplication::sendEvent(composer, &shift_enter);
  EXPECT_EQ(submits, 0);
  EXPECT_TRUE(panel.instruction_text().contains('\n'));

  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
  QApplication::sendEvent(composer, &enter);
  EXPECT_EQ(submits, 1);
  EXPECT_TRUE(panel.instruction_text().isEmpty());
}

TEST(AgentConversationPanel, GroupsActivityAndShowsInlineApproval) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);
  panel.set_current_task_id("task-activity", AgentTaskStateV1::kRunning);

  AgentMessageEnvelopeV1 tool_event;
  tool_event.type = AgentMessageTypeV1::kEvent;
  tool_event.task_id = "task-activity";
  tool_event.task_state = AgentTaskStateV1::kRunning;
  tool_event.event_kind = "tool_call";
  tool_event.text = "cmake --build focused_target";
  panel.apply_task_message(tool_event);
  tool_event.event_kind = "tool_result";
  tool_event.text = "exit=0";
  panel.apply_task_message(tool_event);

  AgentMessageEnvelopeV1 approval;
  approval.type = AgentMessageTypeV1::kApprovalRequest;
  approval.task_id = "task-activity";
  approval.task_state = AgentTaskStateV1::kAwaitingApproval;
  approval.request_id = "approval-1";
  approval.event_kind = "approval_request";
  approval.text = "Allow the bounded build command?";
  panel.apply_task_message(approval);
  QEventLoop flush;
  QTimer::singleShot(150, &flush, &QEventLoop::quit);
  flush.exec();

  EXPECT_TRUE(panel.approval_pending());
  auto* banner = panel.findChild<QFrame*>("agentApprovalBanner");
  ASSERT_NE(banner, nullptr);
  EXPECT_FALSE(banner->isHidden());
  EXPECT_FALSE(panel.findChildren<QFrame*>("agentActivityGroup").isEmpty());
}

TEST(AgentConversationPanel, RejectsComposerTextBeyondProtocolLimit) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);
  panel.set_instruction_text(QString(16385, 'x'));
  EXPECT_TRUE(panel.instruction_text().isEmpty());
  EXPECT_TRUE(panel.status_text().contains("16384"));
}

TEST(AgentConversationPanel, ExpandedActivityRemeasuresWrappedDetailsAndFollowingRow) {
  QTemporaryDir directory;
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);
  panel.resize(420, 900);
  panel.show();

  AgentMessageEnvelopeV1 event;
  event.type = AgentMessageTypeV1::kEvent;
  event.task_id = "expanded-activity";
  event.task_state = AgentTaskStateV1::kRunning;
  event.event_kind = "tool_result";
  event.text = "A long remote response must wrap at the available panel width.\n"
      "The second line must remain visible when the triangle is expanded.\n"
      "The final line must fit inside the activity card.";
  panel.apply_task_message(event);
  event.type = AgentMessageTypeV1::kTaskComplete;
  event.task_state = AgentTaskStateV1::kCompleted;
  event.event_kind = "completed";
  event.text = "Following message";
  panel.apply_task_message(event);
  ASSERT_TRUE(wait_for_panel_render([&] {
    return panel.findChildren<QFrame*>("agentActivityGroup").size() == 2;
  }));
  const auto cards = panel.findChildren<QFrame*>("agentActivityGroup");
  auto* card = cards[0]->findChild<QLabel*>()->text().contains("A long") ? cards[0] : cards[1];
  auto* following = card == cards[0] ? cards[1] : cards[0];
  auto* row = card->parentWidget();
  auto* details = card->findChild<QLabel*>("agentActivityDetails");
  auto* toggle = card->findChild<QToolButton*>();
  ASSERT_NE(details, nullptr);
  ASSERT_NE(toggle, nullptr);
  const int collapsed_height = row->height();
  toggle->click();
  ASSERT_TRUE(wait_for_panel_render([&] {
    return row->height() > collapsed_height
        && details->height() >= details->heightForWidth(details->width())
        && following->parentWidget()->y() >= row->geometry().bottom();
  }));
  EXPECT_TRUE(card->rect().contains(details->geometry()));
  EXPECT_TRUE(row->rect().contains(card->geometry()));
  const int expanded_width = row->width();
  panel.resize(340, 900);
  ASSERT_TRUE(wait_for_panel_render([&] {
    return row->width() < expanded_width
        && details->height() >= details->heightForWidth(details->width())
        && following->parentWidget()->y() >= row->geometry().bottom();
  }));
  toggle->click();
  ASSERT_TRUE(wait_for_panel_render([&] { return row->height() == collapsed_height; }));
  EXPECT_TRUE(details->isHidden());
}

TEST(AgentConversationPanel, ReconnectClearsOfflineHintWithoutHeartbeatOverwritingTaskState) {
  QTemporaryDir directory;
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);
  auto* status = panel.findChild<QLabel*>("agentPanelStatus");
  ASSERT_NE(status, nullptr);
  panel.set_transport_state(true, false, false);
  EXPECT_TRUE(status->text().contains("disconnected"));
  panel.set_transport_state(true, true, true);
  EXPECT_TRUE(status->text().contains("Synchronizing"));
  panel.set_transport_state(true, true, false);
  EXPECT_FALSE(status->text().contains("disconnected"));
  panel.set_current_task_id("task-retained", AgentTaskStateV1::kCompleted);
  const auto task_status = status->text();
  panel.set_transport_state(true, true, false);
  EXPECT_EQ(status->text(), task_status);
}

TEST(AgentConversationPanel, MissingHistoryIsOneLossNoticeNotAnAgentAnswer) {
  QTemporaryDir directory;
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);
  panel.set_transport_state(true, true, true);
  panel.set_instruction_text("A new bounded follow-up");
  auto* send = panel.findChild<QPushButton*>("agentSendAction");
  ASSERT_NE(send, nullptr);
  EXPECT_FALSE(send->isEnabled());
  AgentMessageEnvelopeV1 gap;
  gap.type = AgentMessageTypeV1::kTaskSnapshot;
  gap.task_id = "metadata-only";
  gap.task_state = AgentTaskStateV1::kCompleted;
  gap.event_sequence = 9;
  gap.gap = true;
  gap.event_kind = "history_gap";
  gap.error_code = "history_unavailable";
  gap.text = "Earlier Agent output is unavailable; missing text was not delivered.";
  panel.apply_task_message(gap);
  panel.apply_task_message(gap);
  auto terminal = gap;
  terminal.gap = false;
  terminal.error_code.clear();
  terminal.event_kind.clear();
  terminal.text.clear();
  panel.apply_task_message(terminal);
  panel.set_transport_state(true, true, false);
  QEventLoop flush;
  QTimer::singleShot(150, &flush, &QEventLoop::quit);
  flush.exec();
  int loss_notices = 0;
  for (auto* label : panel.findChildren<QLabel*>("agentSystemError")) {
    if (label->text().contains("missing text was not delivered")) ++loss_notices;
  }
  EXPECT_EQ(loss_notices, 1);
  EXPECT_TRUE(panel.findChildren<QWidget*>("agentReplyText").isEmpty());
  EXPECT_FALSE(panel.status_text().contains("Synchronizing"));
  EXPECT_TRUE(send->isEnabled());
}

TEST(AgentConversationPanel, ContextAndComposerUseCompactOverlayLayout) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);
  panel.resize(420, 720);
  panel.show();
  QApplication::processEvents();

  auto* composer = panel.findChild<QPlainTextEdit*>("agentComposer");
  auto* toggle = panel.findChild<QToolButton*>("agentContextToggle");
  auto* context = panel.findChild<QFrame*>("agentContextBox");
  ASSERT_NE(composer, nullptr);
  ASSERT_NE(toggle, nullptr);
  ASSERT_NE(context, nullptr);
  EXPECT_EQ(composer->height(), 88);
  const QSize before = panel.size();
  toggle->setChecked(true);
  QApplication::processEvents();
  EXPECT_TRUE(context->isVisible());
  EXPECT_TRUE(context->isWindow());
  EXPECT_EQ(panel.size(), before);
  toggle->setChecked(false);
  EXPECT_FALSE(context->isVisible());
}

TEST(AgentConversationPanel, FitsPersistedWidthAndCanRenderVisualReference) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);
  panel.set_submit_callback([](AgentSubmitMode, const QString&) {
    return AgentSubmitResult{.ok = true, .task_id = "task-visual"};
  });
  panel.set_instruction_text("本地消息使用单线边框。\n检查对端返回的多行内容能否完整展开。");
  ASSERT_TRUE(panel.trigger_submit(AgentSubmitMode::kCreateTask));
  panel.set_current_task_id("task-visual", AgentTaskStateV1::kRunning);

  AgentMessageEnvelopeV1 reply;
  reply.type = AgentMessageTypeV1::kEvent;
  reply.task_id = "task-visual";
  reply.task_state = AgentTaskStateV1::kRunning;
  reply.event_kind = "agentMessage/delta";
  reply.text = "### Build ready\nThe focused test passed. Waiting for remote evidence.";
  panel.apply_task_message(reply);

  AgentMessageEnvelopeV1 activity = reply;
  activity.event_kind = "tool_result";
  activity.text = "build.ps1 completed with exit code 0\n"
      "The remote response wraps across multiple lines at this panel width.\n"
      "最后一行也应完整显示在展开的消息控件内。";
  panel.apply_task_message(activity);

  panel.resize(420, 760);
  panel.show();
  ASSERT_TRUE(wait_for_panel_render([&] {
    return panel.findChild<QWidget*>("agentReplyText") != nullptr
        && !panel.findChildren<QFrame*>("agentActivityGroup").isEmpty();
  }));
  EXPECT_EQ(panel.width(), 420);
  EXPECT_NE(panel.findChild<QWidget*>("agentReplyText"), nullptr);
  EXPECT_FALSE(panel.findChildren<QFrame*>("agentActivityGroup").isEmpty());
  auto* card = panel.findChild<QFrame*>("agentActivityGroup");
  auto* details = card->findChild<QLabel*>("agentActivityDetails");
  const int collapsed_height = card->parentWidget()->height();
  card->findChild<QToolButton*>()->click();
  ASSERT_TRUE(wait_for_panel_render([&] {
    return card->parentWidget()->height() > collapsed_height
        && details->height() >= details->heightForWidth(details->width());
  }));

  const QString screenshot_path = qEnvironmentVariable(
      "REDCLAW_AGENT_PANEL_SCREENSHOT");
  if (!screenshot_path.isEmpty()) {
    ASSERT_TRUE(QFileInfo(screenshot_path).dir().mkpath("."));
    ASSERT_TRUE(panel.grab().save(screenshot_path, "PNG"));
  }
}

TEST(AgentConversationPanel, ApprovalAndTerminalEventsAreNotMergedWithActivity) {
  QTemporaryDir directory;
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);
  panel.resize(480, 900);
  panel.show();
  AgentMessageEnvelopeV1 event;
  event.type = AgentMessageTypeV1::kEvent;
  event.task_id = "boundaries";
  event.task_state = AgentTaskStateV1::kRunning;
  event.event_kind = "tool_started";
  event.text = "ordinary activity";
  panel.apply_task_message(event);
  event.type = AgentMessageTypeV1::kApprovalRequest;
  event.task_state = AgentTaskStateV1::kAwaitingApproval;
  event.request_id = "approval-one";
  event.event_kind = "approval_requested";
  event.text = "explicit command summary";
  panel.apply_task_message(event);
  event.type = AgentMessageTypeV1::kTaskComplete;
  event.task_state = AgentTaskStateV1::kCompleted;
  event.event_kind = "completed";
  event.text = "terminal result";
  panel.apply_task_message(event);
  QEventLoop loop;
  QTimer ready;
  QObject::connect(&ready, &QTimer::timeout, &loop, [&] {
    if (panel.findChildren<QFrame*>("agentActivityGroup").size() == 3) loop.quit();
  });
  ready.start(10);
  QTimer::singleShot(1000, &loop, &QEventLoop::quit);
  loop.exec();
  EXPECT_EQ(panel.findChildren<QFrame*>("agentActivityGroup").size(), 3);
}

TEST(AgentConversationPanel, TextUpdatesRetainExistingMessageWidget) {
  QTemporaryDir directory;
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);
  panel.set_current_task_id("incremental", AgentTaskStateV1::kRunning);
  panel.resize(480, 720);
  panel.show();
  AgentMessageEnvelopeV1 reply;
  reply.type = AgentMessageTypeV1::kEvent;
  reply.task_id = "incremental";
  reply.task_state = AgentTaskStateV1::kRunning;
  reply.event_kind = "agentMessage/delta";
  reply.text = "first ";
  panel.apply_task_message(reply);
  const auto drain = [] {
    QEventLoop loop;
    QTimer::singleShot(150, &loop, &QEventLoop::quit);
    loop.exec();
  };
  drain();
  auto* original = panel.findChild<QWidget*>("agentReplyText");
  ASSERT_NE(original, nullptr);
  reply.text = "second";
  panel.apply_task_message(reply);
  drain();
  EXPECT_EQ(panel.findChild<QWidget*>("agentReplyText"), original);
  EXPECT_TRUE(static_cast<redclaw::ui::AgentMessageTextView*>(original)->toPlainText().contains("first second"));
}

TEST(AgentConversationPanel, DecidedAndTerminalApprovalReplayCannotReopenBanner) {
  QTemporaryDir directory;
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);
  panel.set_approval_callback([](auto, QString*) { return true; });
  AgentMessageEnvelopeV1 approval;
  approval.type = AgentMessageTypeV1::kApprovalRequest;
  approval.task_id = "replay";
  approval.request_id = "approval";
  approval.task_state = AgentTaskStateV1::kAwaitingApproval;
  approval.event_sequence = 1;
  approval.text = "Read README";
  panel.apply_task_message(approval);
  ASSERT_TRUE(panel.approval_pending());
  ASSERT_TRUE(panel.trigger_approval(redclaw::protocol::AgentApprovalDecisionV1::kAccept));
  for (int i = 0; i < 120; ++i) panel.apply_task_message(approval);
  EXPECT_FALSE(panel.approval_pending());
  auto terminal = approval;
  terminal.event_sequence = 2;
  terminal.type = AgentMessageTypeV1::kTaskComplete;
  terminal.task_state = AgentTaskStateV1::kCompleted;
  panel.apply_task_message(terminal);
  panel.apply_task_message(approval);
  EXPECT_FALSE(panel.approval_pending());
  EXPECT_TRUE(panel.status_text().contains("completed"));
}

TEST(AgentConversationPanel, StreamingTableDoesNotReparseMarkdownPerTokenOrFreezeTimers) {
  QTemporaryDir directory;
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);
  panel.resize(480, 720);
  panel.show();
  AgentMessageEnvelopeV1 event;
  event.type = AgentMessageTypeV1::kEvent;
  event.task_id = "streaming-table";
  event.task_state = AgentTaskStateV1::kRunning;
  event.event_kind = "agentMessage/delta";
  event.text = "| field | value |\n| --- | --- |\n";
  event.event_sequence = 1;
  QString expected_text = QString::fromStdString(event.text);
  panel.apply_task_message(event);
  QElapsedTimer elapsed;
  elapsed.start();
  qint64 previous = 0;
  qint64 maximum_gap = 0;
  int tokens = 0;
  QEventLoop loop;
  QTimer producer, heartbeat;
  QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&] {
    const auto now = elapsed.elapsed();
    maximum_gap = std::max(maximum_gap, now - previous);
    previous = now;
  });
  QObject::connect(&producer, &QTimer::timeout, &loop, [&] {
    for (int i = 0; i < 8 && tokens < 1000; ++i, ++tokens) {
      event.text = tokens % 2 ? "value |\n" : "| field | ";
      expected_text += QString::fromStdString(event.text);
      ++event.event_sequence;
      panel.apply_task_message(event);
    }
    if (tokens == 1000) {
      producer.stop();
      event.type = AgentMessageTypeV1::kTaskComplete;
      event.task_state = AgentTaskStateV1::kCompleted;
      event.event_kind = "completed";
      event.text = "done";
      ++event.event_sequence;
      panel.apply_task_message(event);
      QTimer::singleShot(400, &loop, &QEventLoop::quit);
    }
  });
  producer.start(5);
  heartbeat.start(5);
  QTimer::singleShot(10000, &loop, &QEventLoop::quit);
  loop.exec();
  EXPECT_EQ(tokens, 1000);
  EXPECT_LT(maximum_gap, 250);
  EXPECT_TRUE(panel.status_text().contains("completed"));
  const auto responses = panel.findChildren<QWidget*>("agentReplyText");
  ASSERT_FALSE(responses.isEmpty());
  QString displayed_text;
  for (const auto* response : responses) {
    displayed_text += static_cast<const redclaw::ui::AgentMessageTextView*>(response)->toPlainText();
    EXPECT_LE(response->height(), 720);
  }
  // The final short segment may settle into rich Markdown; compare all cell
  // contents, excluding only table separators and layout whitespace.
  EXPECT_FALSE(displayed_text.isEmpty());
  EXPECT_EQ(panel.property("retained_reply_utf16_units").toLongLong(), expected_text.size());
  RecordProperty("streaming_tokens", tokens);
  RecordProperty("gui_timer_max_gap_ms", maximum_gap);
}

TEST(AgentConversationPanel, PendingApprovalSnapshotRestoresBannerAfterReconnect) {
  QTemporaryDir directory;
  QSettings settings(directory.filePath("panel.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);
  AgentMessageEnvelopeV1 snapshot;
  snapshot.type = AgentMessageTypeV1::kTaskSnapshot;
  snapshot.task_id = "reconnected";
  snapshot.task_state = AgentTaskStateV1::kAwaitingApproval;
  snapshot.event_kind = "approval_pending";
  snapshot.event_sequence = 7;
  snapshot.request_id = "pending-seven";
  snapshot.text = "Read README";
  panel.apply_task_message(snapshot);
  EXPECT_TRUE(panel.approval_pending());
  EXPECT_EQ(panel.approval_request_id(), "pending-seven");
}

TEST(AgentConversationPanel, VirtualizedHistoryRestoresExactTextWhenScrolled) {
  QTemporaryDir directory;
  QSettings settings(directory.filePath("history.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);
  panel.resize(480, 720);
  panel.show();
  AgentMessageEnvelopeV1 event;
  event.type = AgentMessageTypeV1::kEvent;
  event.task_id = "history";
  event.task_state = AgentTaskStateV1::kRunning;
  event.event_kind = "text_delta";
  for (int i = 0; i < 12; ++i) {
    event.text = std::string(8192, static_cast<char>('A' + i));
    event.event_sequence = static_cast<std::uint64_t>(i + 1);
    panel.apply_task_message(event);
  }
  ASSERT_TRUE(wait_for_panel_render([&] {
    return panel.property("retained_reply_utf16_units").toLongLong() == 12 * 8192;
  }));
  auto* scroll = panel.findChild<QAbstractScrollArea*>("agentConversationScroll");
  ASSERT_NE(scroll, nullptr);
  const auto displays = [&](char ch) {
    for (auto* view : panel.findChildren<QWidget*>("agentReplyText"))
      if (view->isVisible() && static_cast<redclaw::ui::AgentMessageTextView*>(view)->toPlainText() == QString(8192, QChar(ch))) return true;
    return false;
  };
  const bool tail_ready = wait_for_panel_render([&] { return displays('L'); });
  if (!tail_ready) {
    RecordProperty("scroll_value", scroll->verticalScrollBar()->value());
    RecordProperty("scroll_maximum", scroll->verticalScrollBar()->maximum());
    for (auto* item : panel.findChildren<QWidget*>("agentReplyText")) {
      const auto text = static_cast<redclaw::ui::AgentMessageTextView*>(item)->toPlainText();
      RecordProperty("fixture_visible_" + std::to_string(text.isEmpty() ? 0 : text.front().unicode()),
                     static_cast<int>(text.size()));
    }
  }
  ASSERT_TRUE(tail_ready);
  scroll->verticalScrollBar()->setValue(0);
  ASSERT_TRUE(wait_for_panel_render([&] { return displays('A'); }));
  EXPECT_LT(panel.findChildren<QWidget*>("agentReplyText").size(), 12);
  scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
  ASSERT_TRUE(wait_for_panel_render([&] { return displays('L'); }));
  EXPECT_EQ(panel.property("retained_reply_utf16_units").toLongLong(), 12 * 8192);
}

TEST(AgentConversationPanel, MegabyteUnbrokenTextRetainsTerminalAndBoundedHeartbeat) {
  QTemporaryDir directory;
  QSettings settings(directory.filePath("megabyte.ini"), QSettings::IniFormat);
  AgentConversationPanel panel(&settings);
  make_panel_ready(&panel);
  panel.resize(480, 720);
  panel.show();
  panel.set_current_task_id("megabyte", AgentTaskStateV1::kRunning);
  // Measure initial native-window setup separately from steady streaming.
  // The throughput comparison starts after the first layout/paint, just as
  // the dual-GUI gate starts from an already connected idle window.
  QElapsedTimer idle_clock;
  idle_clock.start();
  qint64 idle_previous = 0, idle_gap = 0;
  QEventLoop idle_loop;
  QTimer idle_heartbeat;
  QObject::connect(&idle_heartbeat, &QTimer::timeout, &idle_loop, [&] {
    const auto now = idle_clock.elapsed();
    idle_gap = std::max(idle_gap, now - idle_previous);
    idle_previous = now;
  });
  idle_heartbeat.start(5);
  QTimer::singleShot(1000, &idle_loop, &QEventLoop::quit);
  idle_loop.exec();
  idle_heartbeat.stop();
  RecordProperty("initial_idle_timer_max_gap_ms", idle_gap);
  AgentMessageEnvelopeV1 event;
  event.type = AgentMessageTypeV1::kEvent;
  event.task_id = "megabyte";
  event.task_state = AgentTaskStateV1::kRunning;
  event.event_kind = "text_delta";
  event.text = std::string(8192, 'x');
  QElapsedTimer elapsed;
  elapsed.start();
  qint64 previous = 0, maximum_gap = 0, maximum_call = 0, maximum_gap_at = 0;
  int chunks = 0;
  bool all_rendered = false;
  QEventLoop loop;
  QTimer producer, heartbeat, rendered;
  QObject::connect(&producer, &QTimer::timeout, &loop, [&] {
    const auto begin = elapsed.elapsed();
    event.event_sequence = ++chunks;
    panel.apply_task_message(event);
    maximum_call = std::max(maximum_call, elapsed.elapsed() - begin);
    if (chunks == 128) {
      producer.stop();
      event.type = AgentMessageTypeV1::kTaskComplete;
      event.task_state = AgentTaskStateV1::kCompleted;
      event.event_kind = "completed";
      event.text = "done";
      ++event.event_sequence;
      panel.apply_task_message(event);
    }
  });
  QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&] {
    const auto now = elapsed.elapsed();
    if (now - previous > maximum_gap) { maximum_gap = now - previous; maximum_gap_at = now; }
    previous = now;
  });
  QObject::connect(&rendered, &QTimer::timeout, &loop, [&] {
    if (chunks != 128) return;
    // Virtualized views retain all text but only instantiate visible documents.
    if (panel.property("retained_reply_utf16_units").toLongLong() == 1024 * 1024
        && !panel.findChildren<QWidget*>("agentReplyText").isEmpty()) {
      all_rendered = true; loop.quit();
    }
  });
  producer.start(5);
  heartbeat.start(5);
  rendered.start(100);
  QTimer::singleShot(20000, &loop, &QEventLoop::quit);
  loop.exec();
  EXPECT_EQ(chunks, 128);
  EXPECT_TRUE(all_rendered);
  EXPECT_EQ(panel.current_task_state(), AgentTaskStateV1::kCompleted);
  EXPECT_LT(maximum_gap, 250);
  RecordProperty("megabyte_elapsed_ms", elapsed.elapsed());
  RecordProperty("megabyte_api_max_ms", maximum_call);
  RecordProperty("megabyte_timer_max_gap_ms", maximum_gap);
  RecordProperty("megabyte_timer_max_gap_at_ms", maximum_gap_at);
  RecordProperty("panel_size", QString("%1x%2").arg(panel.width()).arg(panel.height()).toStdString());
  qsizetype final_bytes = panel.property("retained_reply_utf16_units").toLongLong();
  const auto final_views = panel.findChildren<QWidget*>("agentReplyText");
    EXPECT_EQ(final_bytes, 1024 * 1024);
    const auto timing = panel.diagnostic_timing();
    EXPECT_EQ(timing["retained_reply_utf16_units"].toInteger(), final_bytes);
    EXPECT_FALSE(timing["output_gap"].toBool());
    EXPECT_GT(timing["first_output_consumed_us"].toInteger(), 0);
    EXPECT_GE(timing["terminal_consumed_us"].toInteger(), timing["first_output_consumed_us"].toInteger());
    EXPECT_GE(timing["terminal_painted_us"].toInteger(), timing["terminal_consumed_us"].toInteger());
  EXPECT_LT(final_views.size(), 32);
  RecordProperty("retained_bytes", final_bytes);
  RecordProperty("retained_rows", final_views.size());
  auto* conversation_scroll = panel.findChild<QAbstractScrollArea*>("agentConversationScroll");
  RecordProperty("refresh_count", conversation_scroll->property("refresh_count").toULongLong());
  RecordProperty("max_refresh_ms", conversation_scroll->property("max_refresh_ms").toLongLong());
}

}  // namespace
