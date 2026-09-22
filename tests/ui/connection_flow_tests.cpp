#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include <QApplication>
#include <QElapsedTimer>
#include <QComboBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QFrame>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSize>
#include <QToolButton>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>

#include "redclaw/input/input_module.h"
#include "ui/connection_entry_page.h"
#include "ui/connection_password_panel.h"
#include <QSettings>
#include <QTemporaryDir>
#include "ui/connection_flow_model.h"
#include "ui/connection_progress_page.h"
#include "ui/playback_control_hint_overlay.h"
#include "ui/playback_window_lifecycle.h"
#include "ui/remote_input_capture.h"
#include "ui/stun_server_policy.h"

namespace redclaw::ui {
// Exercise queue scheduling without installing hooks or touching the desktop.
class ControllerRemoteInputCaptureTestPeer {
public:
  static void start(ControllerRemoteInputCapture& capture) {
    capture.active_ = true;
    capture.desktop_geometry_revision_ = 1;
  }
  static void enqueue(ControllerRemoteInputCapture& capture,
                      redclaw::protocol::RemoteInputEventTypeV1 type, std::uint16_t scan) {
    redclaw::protocol::RemoteInputEventV1 event;
    event.type = type; event.scan_code = scan;
    capture.enqueue_critical(event);
  }
  static void flush(ControllerRemoteInputCapture& capture) { capture.flush_batch(); }
  static void synchronize(ControllerRemoteInputCapture& capture) { capture.send_state_sync(); }
};
}  // namespace redclaw::ui

namespace {

TEST(ControllerRemoteInputOrdering, ReleasedModifierSnapshotCannotOvertakeQueuedChord) {
  using Peer = redclaw::ui::ControllerRemoteInputCaptureTestPeer;
  using RemoteType = redclaw::protocol::RemoteInputEventTypeV1;
  using Type = redclaw::input::InputEventType;
  using MessageType = redclaw::protocol::StreamControlMessageTypeV1;
  // Shift+/, Ctrl+C and Ctrl+V, with the periodic sync due before the flush.
  for (const auto [modifier, key] : {std::pair{0x2a, 0x35}, {0x1d, 0x2e}, {0x1d, 0x2f}}) {
    redclaw::input::InputPolicyGate gate;
    redclaw::input::InMemoryInputInjectorBackend backend;
    redclaw::input::RemoteInputSession host(gate, backend);
    host.set_authorized(true);
    ASSERT_TRUE(host.request_active(1000));
    redclaw::ui::ControllerRemoteInputCapture capture(nullptr);
    capture.set_send_message_callback([&](const auto& message, QString*) {
      if (message.type == MessageType::kInputBatch) {
        std::vector<redclaw::input::InputEvent> events;
        for (const auto& remote : message.input_events) {
          redclaw::input::InputEvent event;
          event.type = remote.type == RemoteType::kKeyDown ? Type::kKeyDown : Type::kKeyUp;
          event.scan_code = remote.scan_code;
          events.push_back(event);
        }
        return host.enqueue_batch(message.input_sequence, std::move(events), 1100) && host.drain(1100);
      }
      return host.synchronize_state(message.input_sequence, message.pressed_scan_codes,
                                    message.pressed_mouse_buttons, 1100);
    });
    Peer::start(capture);
    Peer::enqueue(capture, RemoteType::kKeyDown, static_cast<std::uint16_t>(modifier));
    Peer::flush(capture);
    Peer::enqueue(capture, RemoteType::kKeyDown, static_cast<std::uint16_t>(key));
    Peer::enqueue(capture, RemoteType::kKeyUp, static_cast<std::uint16_t>(key));
    Peer::enqueue(capture, RemoteType::kKeyUp, static_cast<std::uint16_t>(modifier));
    Peer::synchronize(capture); // Physical keys are now all up.
    Peer::flush(capture);
    const auto& injected = backend.injected_events();
    ASSERT_EQ(injected.size(), 4U);
    EXPECT_EQ(injected[0].scan_code, modifier);
    EXPECT_EQ(injected[1].scan_code, key);
    EXPECT_EQ(injected[1].type, Type::kKeyDown);
    EXPECT_EQ(injected[2].scan_code, key);
    EXPECT_EQ(injected[3].scan_code, modifier);
    EXPECT_EQ(injected[3].type, Type::kKeyUp);
  }
}

TEST(ControllerRemoteInputOrdering, SnapshotDrainsAllBatchesAndStopsOnSendFailure) {
  using Peer = redclaw::ui::ControllerRemoteInputCaptureTestPeer;
  using Type = redclaw::protocol::RemoteInputEventTypeV1;
  using MessageType = redclaw::protocol::StreamControlMessageTypeV1;
  for (const bool fail : {false, true}) {
    redclaw::ui::ControllerRemoteInputCapture capture(nullptr);
    std::size_t events = 0, snapshots = 0;
    capture.set_send_message_callback([&](const auto& message, QString*) {
      if (message.type == MessageType::kInputBatch) {
        events += message.input_events.size();
        return !fail;
      }
      if (message.type == MessageType::kInputStateSync) {
        ++snapshots;
        EXPECT_EQ(events, 120U);
      }
      return true;
    });
    Peer::start(capture);
    for (int i = 0; i < 60; ++i) {
      Peer::enqueue(capture, Type::kKeyDown, 0x2e);
      Peer::enqueue(capture, Type::kKeyUp, 0x2e);
    }
    Peer::synchronize(capture);
    EXPECT_EQ(snapshots, fail ? 0U : 1U);
    EXPECT_EQ(capture.active(), !fail);
    EXPECT_EQ(capture.queued_critical_event_count(), 0U);
  }
}

TEST(ClipboardShortcutState, RemoteCopyKeepsRemotePasteUntilLocalClipboardChanges) {
  redclaw::ui::ClipboardShortcutState state;
  EXPECT_TRUE(state.should_transfer_local_clipboard(12));
  state.remote_copy(12);
  EXPECT_FALSE(state.should_transfer_local_clipboard(12));
  EXPECT_FALSE(state.should_transfer_local_clipboard(12)); // Multiple remote pastes.
  EXPECT_TRUE(state.should_transfer_local_clipboard(13)); // New local copy wins.
  state.remote_copy(13);
  EXPECT_FALSE(state.should_transfer_local_clipboard(13));
  state.reset();
  EXPECT_TRUE(state.should_transfer_local_clipboard(13));
}

using redclaw::ui::ConnectionFlowEvent;
using redclaw::ui::ConnectionFlowModel;
using redclaw::ui::ConnectionFlowRole;
using redclaw::ui::ConnectionFlowStatus;
using redclaw::ui::ConnectionFlowView;
using redclaw::ui::ConnectionStageState;

TEST(PlaybackWindowLifecycle, ClosingPlaybackRestoresClosedMainWindowWithoutQuittingGui) {
  QWidget main_window;
  QWidget playback_window(nullptr, Qt::Window);
  redclaw::ui::PlaybackWindowLifecycle lifecycle(&playback_window, &main_window);
  int last_window_closed_total = 0;
  const auto last_window_connection = QObject::connect(
      qApp,
      &QGuiApplication::lastWindowClosed,
      [&last_window_closed_total]() { ++last_window_closed_total; });

  main_window.show();
  playback_window.show();
  QApplication::processEvents();
  EXPECT_EQ(playback_window.parentWidget(), nullptr);
  main_window.showMinimized();
  QApplication::processEvents();
  EXPECT_TRUE(playback_window.isVisible());
  EXPECT_FALSE(playback_window.isMinimized());
  main_window.showNormal();
  QApplication::processEvents();

  ASSERT_TRUE(main_window.close());
  ASSERT_FALSE(main_window.isVisible());
  ASSERT_TRUE(playback_window.isVisible());

  ASSERT_TRUE(playback_window.close());
  QApplication::processEvents();

  EXPECT_FALSE(playback_window.isVisible());
  EXPECT_TRUE(main_window.isVisible());
  EXPECT_EQ(last_window_closed_total, 0);
  QObject::disconnect(last_window_connection);
}

TEST(ConnectionFlowModel, AdvancesControllerThroughFourUserStages) {
  ConnectionFlowModel model;
  model.start(ConnectionFlowRole::kController);
  EXPECT_EQ(model.snapshot().view, ConnectionFlowView::kConnecting);
  EXPECT_EQ(model.snapshot().stage_count, 4);
  EXPECT_EQ(model.snapshot().current_stage, 0);
  EXPECT_EQ(model.snapshot().stages[0], ConnectionStageState::kCurrent);

  model.apply(ConnectionFlowEvent::kRemoteDescriptionApplied);
  EXPECT_EQ(model.snapshot().current_stage, 1);
  model.apply(ConnectionFlowEvent::kLocalDescriptionPublished);
  EXPECT_EQ(model.snapshot().current_stage, 2);
  model.apply(ConnectionFlowEvent::kIceConnected);
  EXPECT_EQ(model.snapshot().current_stage, 3);
  model.apply(ConnectionFlowEvent::kChannelOpened);
  EXPECT_EQ(model.snapshot().current_stage, 3);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kRunning);

  model.observe_transport(true, true);
  model.apply(ConnectionFlowEvent::kFirstFrameReady);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kComplete);
  for (const auto state : model.snapshot().stages) {
    EXPECT_EQ(state, ConnectionStageState::kComplete);
  }
}

TEST(ConnectionFlowModel, AdvancesHostAndRequiresRealStreamCompletion) {
  ConnectionFlowModel model;
  model.start(ConnectionFlowRole::kHost);
  EXPECT_EQ(model.snapshot().view, ConnectionFlowView::kWaiting);

  model.apply(ConnectionFlowEvent::kLocalDescriptionPublished);
  EXPECT_EQ(model.snapshot().current_stage, 1);
  model.apply(ConnectionFlowEvent::kRemoteDescriptionApplied);
  EXPECT_EQ(model.snapshot().current_stage, 2);
  model.apply(ConnectionFlowEvent::kIceConnected);
  EXPECT_EQ(model.snapshot().current_stage, 3);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kRunning);

  model.observe_transport(true, true);
  model.apply(ConnectionFlowEvent::kFirstFrameReady);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kComplete);
}

TEST(ConnectionFlowModel, DuplicateAndOutOfOrderEventsNeverRegressProgress) {
  ConnectionFlowModel model;
  model.start(ConnectionFlowRole::kController);
  model.apply(ConnectionFlowEvent::kLocalDescriptionPublished);
  EXPECT_EQ(model.snapshot().current_stage, 2);
  model.apply(ConnectionFlowEvent::kRemoteDescriptionApplied);
  model.apply(ConnectionFlowEvent::kRemoteDescriptionApplied);
  EXPECT_EQ(model.snapshot().current_stage, 2);
}

TEST(ConnectionFlowModel, DisconnectedHostRejectsOldCumulativeFramesAcrossRetry) {
  ConnectionFlowModel model;
  model.start(ConnectionFlowRole::kHost);
  model.observe_transport(true, true);
  model.observe_host_frames(73414, 35868, 35866);
  ASSERT_EQ(model.snapshot().status, ConnectionFlowStatus::kComplete);

  model.observe_transport(false, false);
  ASSERT_EQ(model.snapshot().status, ConnectionFlowStatus::kDisconnected);
  for (int tick = 0; tick < 100; ++tick) {
    model.observe_transport(false, false);
    model.observe_host_frames(73414, 35868, 35866);
    model.apply(ConnectionFlowEvent::kFirstFrameReady);  // Late cached GUI frame.
    EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kDisconnected);
    EXPECT_EQ(model.snapshot().attempt, 1);
  }
  model.apply(ConnectionFlowEvent::kAutomaticRetry);
  model.observe_host_frames(73414, 35868, 35866);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kRunning);
  model.observe_transport(true, true);
  model.observe_host_frames(73414, 35868, 35866);
  EXPECT_NE(model.snapshot().status, ConnectionFlowStatus::kComplete);
  // A static retained capture is valid, but encode AND send must be new.
  model.observe_host_frames(73414, 35869, 35866);
  EXPECT_NE(model.snapshot().status, ConnectionFlowStatus::kComplete);
  model.observe_host_frames(73414, 35869, 35867);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kComplete);
}

TEST(ConnectionFlowModel, RuntimeCounterResetAndInitialClosedStateAreSafe) {
  ConnectionFlowModel model;
  model.start(ConnectionFlowRole::kHost);
  model.observe_transport(false, false);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kRunning);
  model.observe_transport(true, true);
  model.observe_host_frames(10, 8, 7);
  model.observe_transport(false, true);
  model.apply(ConnectionFlowEvent::kAutomaticRetry);
  model.observe_host_frames(0, 0, 0);
  model.observe_transport(true, true);
  model.observe_host_frames(1, 1, 1);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kComplete);
  model.begin_stopping();
  model.observe_transport(false, false);
  model.observe_transport(true, true);
  model.observe_host_frames(2, 2, 2);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kStopping);
  model.reset();
  model.observe_transport(true, true);
  model.apply(ConnectionFlowEvent::kFirstFrameReady);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kIdle);
}

TEST(ConnectionProgressPage, TransportLossClearsSuccessForBothRolesWithoutManualRetry) {
  for (const auto role : {ConnectionFlowRole::kHost, ConnectionFlowRole::kController}) {
    ConnectionFlowModel model;
    redclaw::ui::ConnectionProgressPage page(role);
    model.start(role);
    model.apply(ConnectionFlowEvent::kFirstFrameReady);
    EXPECT_NE(model.snapshot().status, ConnectionFlowStatus::kComplete);
    model.observe_transport(true, true);
    model.apply(ConnectionFlowEvent::kFirstFrameReady);
    ASSERT_EQ(model.snapshot().status, ConnectionFlowStatus::kComplete);
    // Required media channel can disappear before the next aggregate ICE poll.
    model.observe_transport(true, false);
    page.set_snapshot(model.snapshot());
    EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kDisconnected);
    EXPECT_TRUE(page.phase_text().contains(role == ConnectionFlowRole::kHost
        ? "waiting for a Controller" : "reconnecting automatically"));
    EXPECT_TRUE(page.retry_button()->isHidden());
    EXPECT_EQ(page.exit_button()->text(), "Exit");
    model.observe_transport(true, true);
    EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kRunning);
    model.apply(ConnectionFlowEvent::kFirstFrameReady);
    EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kComplete);
  }
}

TEST(ConnectionFlowModel, RetryFailureAndExitHaveStableActions) {
  ConnectionFlowModel model;
  model.start(ConnectionFlowRole::kController);
  model.apply(ConnectionFlowEvent::kRemoteDescriptionApplied);
  model.apply(ConnectionFlowEvent::kFatalFailure, "ICE path failed");
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kFailed);
  EXPECT_EQ(model.snapshot().stages[1], ConnectionStageState::kFailed);
  EXPECT_TRUE(model.snapshot().can_retry);
  EXPECT_TRUE(model.snapshot().can_exit);

  model.apply(ConnectionFlowEvent::kAutomaticRetry);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kRunning);
  EXPECT_EQ(model.snapshot().attempt, 2);
  EXPECT_EQ(model.snapshot().current_stage, 0);
  EXPECT_EQ(model.snapshot().stages[0], ConnectionStageState::kCurrent);

  model.begin_stopping();
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kStopping);
  EXPECT_FALSE(model.snapshot().can_exit);
  model.apply(ConnectionFlowEvent::kAutomaticRetry);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kStopping);
  EXPECT_EQ(model.snapshot().attempt, 2);
  model.apply(ConnectionFlowEvent::kRetryWaiting);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kStopping);
  model.apply(ConnectionFlowEvent::kExitCompleted);
  EXPECT_EQ(model.snapshot().view, ConnectionFlowView::kPreConnection);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kIdle);
  model.apply(ConnectionFlowEvent::kRetryWaiting);
  model.apply(ConnectionFlowEvent::kAutomaticRetry);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kIdle);
}

TEST(ConnectionFlowModel, UnexpectedExitAfterSuccessFailsTheLiveStage) {
  ConnectionFlowModel model;
  model.start(ConnectionFlowRole::kHost);
  model.observe_transport(true, true);
  model.apply(ConnectionFlowEvent::kFirstFrameReady);
  ASSERT_EQ(model.snapshot().status, ConnectionFlowStatus::kComplete);

  model.apply(ConnectionFlowEvent::kFatalFailure, "Runtime exited unexpectedly");
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kFailed);
  EXPECT_EQ(model.snapshot().stages[3], ConnectionStageState::kFailed);
  EXPECT_TRUE(model.snapshot().can_retry);
  EXPECT_TRUE(model.snapshot().can_exit);
}

TEST(ConnectionFlowModel, FatalFailureStaysOnEveryStageForBothRoles) {
  for (const auto role : {ConnectionFlowRole::kController, ConnectionFlowRole::kHost}) {
    for (int failed_stage = 0; failed_stage < 4; ++failed_stage) {
      ConnectionFlowModel model;
      model.start(role);
      if (failed_stage >= 1) {
        model.apply(role == ConnectionFlowRole::kHost
            ? ConnectionFlowEvent::kLocalDescriptionPublished
            : ConnectionFlowEvent::kRemoteDescriptionApplied);
      }
      if (failed_stage >= 2) {
        model.apply(role == ConnectionFlowRole::kHost
            ? ConnectionFlowEvent::kRemoteDescriptionApplied
            : ConnectionFlowEvent::kLocalDescriptionPublished);
      }
      if (failed_stage >= 3) {
        model.apply(ConnectionFlowEvent::kIceConnected);
      }

      model.apply(ConnectionFlowEvent::kFatalFailure, "stage failure");
      EXPECT_EQ(model.snapshot().current_stage, failed_stage);
      EXPECT_EQ(model.snapshot().stages[failed_stage], ConnectionStageState::kFailed);
      EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kFailed);
      EXPECT_TRUE(model.snapshot().can_retry);
      EXPECT_TRUE(model.snapshot().can_exit);
    }
  }
}

TEST(ConnectionFlowLineAdapter, SeparatesAutomaticRepairFromFatalFailure) {
  const auto retry = redclaw::ui::classify_connection_flow_line(
      u"Runtime rebuilding signaling session role=controller attempt=2",
      ConnectionFlowRole::kController);
  ASSERT_TRUE(retry.matched);
  EXPECT_EQ(retry.event, ConnectionFlowEvent::kAutomaticRetry);

  const auto continuing = redclaw::ui::classify_connection_flow_line(
      u"Runtime ICE state failed; DHT retry remains active role=controller",
      ConnectionFlowRole::kController);
  EXPECT_TRUE(continuing.matched);
  EXPECT_EQ(continuing.event, ConnectionFlowEvent::kRetryWaiting);

  const auto incomplete = redclaw::ui::classify_connection_flow_line(
      u"Runtime ICE state failed before DHT signaling completed; continuing role=controller",
      ConnectionFlowRole::kController);
  EXPECT_TRUE(incomplete.matched);
  EXPECT_EQ(incomplete.event, ConnectionFlowEvent::kRetryWaiting);

  const auto failed = redclaw::ui::classify_connection_flow_line(
      u"Runtime ICE state failed role=controller",
      ConnectionFlowRole::kController);
  ASSERT_TRUE(failed.matched);
  EXPECT_EQ(failed.event, ConnectionFlowEvent::kFatalFailure);
}

TEST(ConnectionEntryPage, MakesReadOnlyAndEditableCodesVisiblyDistinct) {
  redclaw::ui::ConnectionEntryPage page;
  page.set_local_code("AB12CD34");
  ASSERT_NE(page.local_code_display(), nullptr);
  ASSERT_NE(page.peer_code_input(), nullptr);
  EXPECT_EQ(page.local_code_display()->focusPolicy(), Qt::NoFocus);
  EXPECT_FALSE(page.local_code_display()->inherits("QLineEdit"));
  EXPECT_TRUE(page.peer_code_input()->isEnabled());
  EXPECT_TRUE(page.peer_code_input()->focusPolicy() != Qt::NoFocus);
  EXPECT_FALSE(page.wait_button()->isEnabled());
  EXPECT_EQ(page.findChild<QLabel*>("readOnlyBadge"), nullptr);
  const auto* local_card = page.findChild<QFrame*>("localCodeCard");
  ASSERT_NE(local_card, nullptr);
  EXPECT_EQ(local_card->height(), page.peer_code_input()->height());

  page.set_peer_code("ab12 cd34");
  EXPECT_EQ(page.peer_code(), "AB12CD34");
  EXPECT_FALSE(page.connect_button()->isEnabled());
  page.findChild<QLineEdit*>("peerConnectionPassword")->setText("fixture password");
  EXPECT_TRUE(page.connect_button()->isEnabled());
  const auto* helper = page.findChild<QLabel*>("fieldHelper");
  ASSERT_NE(helper, nullptr);
  EXPECT_TRUE(helper->isHidden());
}

TEST(ConnectionEntryPage, PasswordSettingsEncryptConfirmRememberAndForget) {
  QTemporaryDir directory;
  QSettings settings(directory.filePath("passwords.ini"), QSettings::IniFormat);
  redclaw::ui::ConnectionEntryPage page;
  page.password_panel()->set_settings(&settings);
  page.set_local_code("AB12CD34"); page.set_peer_code("EF56GH78");
  auto* local = page.findChild<QLineEdit*>("localConnectionPassword");
  auto* confirm = page.findChild<QLineEdit*>("confirmConnectionPassword");
  auto* peer = page.findChild<QLineEdit*>("peerConnectionPassword");
  ASSERT_EQ(local->echoMode(), QLineEdit::Password);
  ASSERT_EQ(peer->echoMode(), QLineEdit::Password);
  local->setText(" exact Password "); confirm->setText("different");
  page.findChild<QPushButton*>("saveConnectionPassword")->click();
  EXPECT_FALSE(page.wait_button()->isEnabled());
  confirm->setText(local->text());
  page.findChild<QPushButton*>("saveConnectionPassword")->click();
  EXPECT_FALSE(page.wait_button()->isEnabled());
  QEventLoop save_loop;
  QTimer heartbeat; int ticks = 0;
  QObject::connect(&heartbeat, &QTimer::timeout, [&] {
    ++ticks;
    if (page.password_panel()->host_ready()) save_loop.quit();
  });
  heartbeat.start(5);
  QTimer::singleShot(10000, &save_loop, &QEventLoop::quit);
  save_loop.exec();
  EXPECT_GT(ticks, 1);
  EXPECT_TRUE(page.wait_button()->isEnabled());
  EXPECT_TRUE(local->text().isEmpty());
  EXPECT_FALSE(settings.value("connectionAuth/v1/host").toString().contains("Password"));
  peer->setText(" exact Password ");
  redclaw::security::ConnectionCredential credential; QString error;
  ASSERT_TRUE(page.password_panel()->prepare(false, &credential, &error));
  EXPECT_EQ(credential.secret, " exact Password ");
  EXPECT_FALSE(settings.contains("connectionAuth/v1/peers/EF56GH78"));
  page.password_panel()->accepted();
  EXPECT_TRUE(settings.contains("connectionAuth/v1/peers/EF56GH78"));
  page.set_actions_enabled(false);
  EXPECT_FALSE(local->isEnabled()); EXPECT_FALSE(peer->isEnabled());
  page.set_actions_enabled(true);
  page.set_peer_code("ZZ11YY22"); EXPECT_TRUE(peer->text().isEmpty());
  page.set_peer_code("EF56GH78"); EXPECT_EQ(peer->text(), " exact Password ");
  page.findChild<QPushButton*>("forgetConnectionPassword")->click();
  EXPECT_TRUE(peer->text().isEmpty());
  EXPECT_FALSE(settings.contains("connectionAuth/v1/peers/EF56GH78"));
  redclaw::ui::ConnectionEntryPage restored;
  restored.password_panel()->set_settings(&settings); restored.set_local_code("AB12CD34");
  EXPECT_TRUE(restored.wait_button()->isEnabled());
  settings.setValue("connectionAuth/v1/host", "corrupted");
  restored.password_panel()->set_settings(&settings);
  EXPECT_FALSE(restored.wait_button()->isEnabled());
}

TEST(ConnectionEntryPage, PasswordStorageWriteFailureKeepsWaitDisabled) {
  QTemporaryDir directory;
  QSettings settings(directory.path(), QSettings::IniFormat); // A directory is not a writable settings file.
  redclaw::ui::ConnectionEntryPage page;
  page.password_panel()->set_settings(&settings); page.set_local_code("AB12CD34");
  page.findChild<QLineEdit*>("localConnectionPassword")->setText("password");
  page.findChild<QLineEdit*>("confirmConnectionPassword")->setText("password");
  page.findChild<QPushButton*>("saveConnectionPassword")->click();
  QEventLoop loop; QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&] { if (page.password_panel()->isEnabled()) loop.quit(); });
  timer.start(5); QTimer::singleShot(10000, &loop, &QEventLoop::quit); loop.exec();
  EXPECT_FALSE(page.password_panel()->host_ready());
  EXPECT_FALSE(page.wait_button()->isEnabled());
  EXPECT_TRUE(page.findChild<QLabel*>("connectionPasswordStatus")->text().contains("Could not"));
}

TEST(ConnectionEntryPage, InvalidPeerCodeCannotStartConnection) {
  redclaw::ui::ConnectionEntryPage page;
  page.set_peer_code("ABC");
  EXPECT_FALSE(page.connect_button()->isEnabled());
  page.set_peer_code("ABC!2345");
  EXPECT_FALSE(page.connect_button()->isEnabled());
  const auto* helper = page.findChild<QLabel*>("fieldHelper");
  ASSERT_NE(helper, nullptr);
  EXPECT_EQ(helper->text(), "Use only letters A-Z and numbers 0-9.");
  EXPECT_FALSE(helper->isHidden());
}

TEST(ConnectionEntryPage, KeepsCollapsedSettingsDirectlyBelowConnectionCard) {
  redclaw::ui::ConnectionEntryPage page;
  page.resize(800, 600);
  page.show();
  QApplication::processEvents();

  ASSERT_NE(page.network_settings_toggle(), nullptr);
  ASSERT_NE(page.network_settings_widget(), nullptr);
  EXPECT_EQ(page.sizePolicy().verticalPolicy(), QSizePolicy::Fixed);
  EXPECT_GE(page.network_settings_layout()->contentsMargins().bottom(), 18);
  EXPECT_FALSE(page.network_settings_toggle()->isChecked());
  EXPECT_TRUE(page.network_settings_widget()->isHidden());
  const int settings_gap =
      page.network_settings_toggle()->geometry().top() - page.primary_card()->geometry().bottom();
  EXPECT_GE(settings_gap, 0);
  EXPECT_LE(settings_gap, 16);

  const int collapsed_height = page.sizeHint().height();
  page.network_settings_toggle()->setChecked(true);
  QApplication::processEvents();
  EXPECT_FALSE(page.network_settings_widget()->isHidden());
  EXPECT_GT(page.sizeHint().height(), collapsed_height);
}

TEST(ConnectionEntryPage, NetworkExitWheelDoesNotChangeSelection) {
  redclaw::ui::PageScrollComboBox combo;
  combo.addItems({"Automatic", "Ethernet", "Wi-Fi"});
  combo.setCurrentIndex(1);
  QWheelEvent wheel(
      QPointF(8.0, 8.0),
      QPointF(8.0, 8.0),
      QPoint(),
      QPoint(0, 120),
      Qt::NoButton,
      Qt::NoModifier,
      Qt::NoScrollPhase,
      false);

  QApplication::sendEvent(&combo, &wheel);
  EXPECT_EQ(combo.currentIndex(), 1);
  EXPECT_FALSE(wheel.isAccepted());
}

TEST(ControllerRemoteInputCapture, ComputesLetterboxedContentRectangle) {
  QWidget canvas;
  canvas.resize(800, 600);
  redclaw::ui::ControllerRemoteInputCapture capture(&canvas);
  capture.set_remote_frame_size(QSize(1920, 1080));

  EXPECT_EQ(capture.content_rect(), QRect(0, 75, 800, 450));
  std::uint16_t x = 0;
  std::uint16_t y = 0;
  EXPECT_FALSE(capture.map_content_position(QPointF(400, 20), &x, &y));
  EXPECT_FALSE(capture.map_content_position(QPointF(400, 560), &x, &y));
  EXPECT_TRUE(capture.map_content_position(QPointF(0, 75), &x, &y));
  EXPECT_EQ(x, 0);
  EXPECT_EQ(y, 0);
  EXPECT_TRUE(capture.map_content_position(QPointF(799, 524), &x, &y));
  EXPECT_EQ(x, 65535);
  EXPECT_EQ(y, 65535);
  EXPECT_FALSE(capture.keyboard_target_is_active());
  capture.set_remote_frame_size(QSize());
  EXPECT_TRUE(capture.content_rect().isEmpty());
}

TEST(ControllerRemoteInputCapture, RestoresSameFrameGeometryAfterReconnectReset) {
  QWidget canvas;
  canvas.resize(800, 600);
  redclaw::ui::ControllerRemoteInputCapture capture(&canvas);
  const QSize remote_frame_size(1920, 1080);

  capture.set_remote_frame_size(remote_frame_size);
  ASSERT_FALSE(capture.content_rect().isEmpty());

  capture.set_remote_frame_size(QSize());
  ASSERT_TRUE(capture.content_rect().isEmpty());

  capture.set_remote_frame_size(remote_frame_size);
  EXPECT_EQ(capture.remote_frame_size(), remote_frame_size);
  EXPECT_EQ(capture.content_rect(), QRect(0, 75, 800, 450));
}

TEST(ControllerRemoteInputCapture, SuppressesOnlyRedClawInjectedMouseLoopback) {
  constexpr auto marker = redclaw::input::kRedClawInputExtraInfo;
  EXPECT_TRUE(redclaw::ui::ControllerRemoteInputCapture::should_suppress_mouse_loopback(
      QEvent::MouseMove, marker));
  EXPECT_TRUE(redclaw::ui::ControllerRemoteInputCapture::should_suppress_mouse_loopback(
      QEvent::MouseButtonPress, marker));
  EXPECT_TRUE(redclaw::ui::ControllerRemoteInputCapture::should_suppress_mouse_loopback(
      QEvent::MouseButtonDblClick, marker));
  EXPECT_TRUE(redclaw::ui::ControllerRemoteInputCapture::should_suppress_mouse_loopback(
      QEvent::MouseButtonRelease, marker));
  EXPECT_TRUE(redclaw::ui::ControllerRemoteInputCapture::should_suppress_mouse_loopback(
      QEvent::Wheel, marker));

  EXPECT_FALSE(redclaw::ui::ControllerRemoteInputCapture::should_suppress_mouse_loopback(
      QEvent::MouseMove, marker ^ 1U));
  EXPECT_FALSE(redclaw::ui::ControllerRemoteInputCapture::should_suppress_mouse_loopback(
      QEvent::KeyPress, marker));
}

TEST(ControllerRemoteInputCapture, CombinesLocalSuspensionReasonsWithoutEndingIntent) {
  QWidget canvas;
  redclaw::ui::ControllerRemoteInputCapture capture(&canvas);
  int forwarding_changes = 0;
  capture.set_forwarding_changed_callback([&forwarding_changes]() { ++forwarding_changes; });

  capture.set_local_suspension(
      redclaw::ui::LocalInputSuspensionReason::kWindowInactive,
      true);
  capture.set_local_suspension(
      redclaw::ui::LocalInputSuspensionReason::kGeometryTransaction,
      true);
  capture.set_local_suspension(
      redclaw::ui::LocalInputSuspensionReason::kLocalUiFocus,
      true);
  EXPECT_EQ(
      capture.local_suspension_reason(),
      "window_inactive,geometry_transaction,local_ui_focus");
  EXPECT_FALSE(capture.input_forwarding());
  capture.set_local_suspension(
      redclaw::ui::LocalInputSuspensionReason::kWindowInactive,
      false);
  EXPECT_EQ(capture.local_suspension_reason(), "geometry_transaction,local_ui_focus");
  capture.set_local_suspension(
      redclaw::ui::LocalInputSuspensionReason::kGeometryTransaction,
      false);
  EXPECT_EQ(capture.local_suspension_reason(), "local_ui_focus");
  capture.set_local_suspension(
      redclaw::ui::LocalInputSuspensionReason::kLocalUiFocus,
      false);
  EXPECT_TRUE(capture.local_suspension_reason().isEmpty());
  EXPECT_EQ(forwarding_changes, 6);
  EXPECT_FALSE(capture.control_enabled());
}

TEST(ControllerRemoteInputCapture, LocalPanelFocusOwnsKeyboardUntilCanvasClick) {
  QWidget playback_window;
  QWidget canvas(&playback_window);
  QPlainTextEdit local_editor(&playback_window);
  canvas.setGeometry(0, 0, 600, 500);
  local_editor.setGeometry(600, 0, 240, 500);
  playback_window.resize(840, 500);
  playback_window.show();
  canvas.show();
  local_editor.show();
  QApplication::processEvents();

  redclaw::ui::ControllerRemoteInputCapture capture(&canvas);
  QMouseEvent local_press(
      QEvent::MouseButtonPress, QPointF(20, 20), QPointF(20, 20),
      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&local_editor, &local_press);
  EXPECT_TRUE(capture.local_suspension_reason().contains("local_ui_focus"));
  EXPECT_FALSE(capture.keyboard_target_is_active());

  QMouseEvent canvas_press(
      QEvent::MouseButtonPress, QPointF(100, 100), QPointF(100, 100),
      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&canvas, &canvas_press);
  EXPECT_FALSE(capture.local_suspension_reason().contains("local_ui_focus"));
}

TEST(ControllerRemoteInputCapture, DisabledCanvasClickIsConsumedAndCounted) {
  QWidget playback_window;
  QWidget canvas(&playback_window);
  playback_window.resize(800, 600);
  canvas.setGeometry(0, 0, 800, 600);
  playback_window.show();
  canvas.show();
  QApplication::processEvents();
  redclaw::ui::ControllerRemoteInputCapture capture(&canvas);
  int hints = 0;
  int sent = 0;
  capture.set_blocked_click_callback([&hints]() { ++hints; });
  capture.set_send_message_callback(
      [&sent](const redclaw::protocol::StreamControlMessageV1&, QString*) {
        ++sent;
        return true;
      });
  QMouseEvent press(
      QEvent::MouseButtonPress,
      QPointF(400, 300),
      QPointF(400, 300),
      Qt::LeftButton,
      Qt::LeftButton,
      Qt::NoModifier);

  EXPECT_TRUE(QApplication::sendEvent(&canvas, &press));
  EXPECT_EQ(hints, 1);
  EXPECT_EQ(capture.blocked_control_click_hint_count(), 1U);
  EXPECT_EQ(sent, 0);
}

TEST(PlaybackControlHintOverlay, IsMouseTransparentAndReusesOneTimedSurface) {
  QWidget playback_window;
  QWidget canvas(&playback_window);
  playback_window.resize(800, 600);
  canvas.setGeometry(0, 0, 800, 520);
  playback_window.show();
  canvas.show();
  QApplication::processEvents();

  redclaw::ui::PlaybackControlHintOverlay overlay(&canvas);
  overlay.show_hint(QString::fromUtf8("未开始控制，请点击 Start Control"), 2000);
  EXPECT_TRUE(overlay.testAttribute(Qt::WA_TransparentForMouseEvents));
  EXPECT_EQ(overlay.focusPolicy(), Qt::NoFocus);
  EXPECT_TRUE(overlay.isVisible());
  EXPECT_EQ(overlay.message(), QString::fromUtf8("未开始控制，请点击 Start Control"));
  overlay.show_hint(QString::fromUtf8("当前为仅观看模式"), 2000);
  EXPECT_TRUE(overlay.isVisible());
  EXPECT_EQ(overlay.message(), QString::fromUtf8("当前为仅观看模式"));
  overlay.hide_hint();
  EXPECT_FALSE(overlay.isVisible());
}

TEST(PlaybackControlHintOverlay, FrozenFrameHintCancelsPreviousTimeout) {
  QWidget canvas;
  canvas.resize(800, 520);
  canvas.show();
  redclaw::ui::PlaybackControlHintOverlay overlay(&canvas);
  overlay.show_hint("Control is paused", 1);
  overlay.show_hint("Disconnected. Last frame retained.", 0);
  QEventLoop wait;
  QTimer::singleShot(30, &wait, &QEventLoop::quit);
  wait.exec();
  EXPECT_TRUE(overlay.isVisible());
  EXPECT_EQ(overlay.message(), "Disconnected. Last frame retained.");
  overlay.show_hint("Click Start Control", 1);
  EXPECT_EQ(overlay.message(), "Disconnected. Last frame retained.");
  overlay.hide_persistent_hint();
  EXPECT_FALSE(overlay.isVisible());
  overlay.show_hint("Control not started", 2000);
  overlay.hide_persistent_hint();
  EXPECT_TRUE(overlay.isVisible());
}

#if defined(_WIN32)
TEST(ControllerRemoteInputCapture, ClipboardCallbackWithholdsPasteAndRepeatUntilKeyUp) {
  QWidget window;
  QWidget canvas(&window);
  window.resize(640, 480); canvas.setGeometry(0, 0, 640, 480);
  window.show(); canvas.show(); QApplication::processEvents();
  const auto hwnd = reinterpret_cast<HWND>(window.winId());
  SetForegroundWindow(hwnd); QApplication::processEvents();
  if (GetForegroundWindow() != hwnd) GTEST_SKIP() << "Foreground ownership unavailable for the input callback fixture.";
  redclaw::ui::ControllerRemoteInputCapture capture(&canvas, &window);
  capture.set_remote_frame_size({640, 480}); capture.set_desktop_geometry_revision(1);
  std::vector<redclaw::protocol::StreamControlMessageV1> sent;
  capture.set_send_message_callback([&](const auto& message, QString*) { sent.push_back(message); return true; });
  unsigned pastes = 0;
  capture.set_clipboard_paste_callback([&](std::uint32_t) {
    ++pastes; capture.set_local_suspension(redclaw::ui::LocalInputSuspensionReason::kWorkspaceTransfer, true);
  });
  QString error; ASSERT_TRUE(capture.activate(&error)) << error.toStdString();
  ASSERT_TRUE(capture.clipboard_paste_context_valid());
  const auto key = [&](UINT message, DWORD vk, DWORD scan) {
    KBDLLHOOKSTRUCT event{}; event.vkCode = vk; event.scanCode = scan;
    return capture.handle_low_level_keyboard(message, reinterpret_cast<std::uintptr_t>(&event));
  };
  EXPECT_EQ(key(WM_KEYDOWN, VK_LCONTROL, 0x1d), 1);
  EXPECT_EQ(key(WM_KEYDOWN, 'V', 0x2f), 1); EXPECT_EQ(pastes, 1U);
  EXPECT_TRUE(capture.control_enabled()); EXPECT_FALSE(capture.input_forwarding());
  EXPECT_TRUE(capture.clipboard_paste_context_valid());
  EXPECT_EQ(key(WM_KEYDOWN, 'V', 0x2f), 1); EXPECT_EQ(pastes, 1U);
  capture.set_local_suspension(redclaw::ui::LocalInputSuspensionReason::kWorkspaceTransfer, false);
  EXPECT_EQ(key(WM_KEYDOWN, 'V', 0x2f), 1); EXPECT_EQ(pastes, 1U);
  EXPECT_EQ(key(WM_KEYUP, 'V', 0x2f), 1);
  for (const auto& message : sent) {
    EXPECT_NE(message.type, redclaw::protocol::StreamControlMessageTypeV1::kInputReleaseAll);
    for (const auto& event : message.input_events) EXPECT_NE(event.virtual_key, 'V');
  }
  capture.set_local_suspension(redclaw::ui::LocalInputSuspensionReason::kLocalUiFocus, true);
  EXPECT_FALSE(capture.clipboard_paste_context_valid());
  EXPECT_EQ(key(WM_KEYDOWN, 'V', 0x2f), 0); EXPECT_EQ(pastes, 1U);
  capture.pause(false, "clipboard callback fixture complete");
  // Calls the native callback directly; does not inject keys or publish clipboard data.
}
TEST(ControllerRemoteInputCapture, LocalSuspensionSendsEmptySyncWithoutEndingControl) {
  QWidget playback_window;
  QWidget canvas(&playback_window);
  canvas.resize(800, 600);
  redclaw::ui::ControllerRemoteInputCapture capture(&canvas);
  capture.set_remote_frame_size(QSize(1920, 1080));
  capture.set_desktop_geometry_revision(7);
  std::vector<redclaw::protocol::StreamControlMessageV1> sent;
  capture.set_send_message_callback(
      [&sent](const redclaw::protocol::StreamControlMessageV1& message, QString*) {
        sent.push_back(message);
        return true;
      });

  QString error;
  ASSERT_TRUE(capture.activate(&error)) << error.toStdString();
  EXPECT_TRUE(capture.control_enabled());
  EXPECT_FALSE(capture.input_forwarding());
  capture.set_local_suspension(
      redclaw::ui::LocalInputSuspensionReason::kGeometryTransaction,
      true);
  ASSERT_FALSE(sent.empty());
  EXPECT_EQ(
      sent.back().type,
      redclaw::protocol::StreamControlMessageTypeV1::kInputStateSync);
  EXPECT_TRUE(sent.back().pressed_scan_codes.empty());
  EXPECT_EQ(sent.back().pressed_mouse_buttons, 0U);
  EXPECT_EQ(sent.back().desktop_geometry_revision, 7U);
  EXPECT_TRUE(capture.control_enabled());
  const std::size_t release_count = sent.size();
  capture.set_local_suspension(
      redclaw::ui::LocalInputSuspensionReason::kLocalUiFocus,
      true);
  capture.set_local_suspension(
      redclaw::ui::LocalInputSuspensionReason::kLocalUiFocus,
      true);
  EXPECT_EQ(sent.size(), release_count);
  capture.pause(false, "test complete");
}
TEST(ControllerRemoteInputCapture, NewControlGrantRetainsAnExistingTransferPause) {
  QWidget canvas;
  redclaw::ui::ControllerRemoteInputCapture capture(&canvas);
  capture.set_remote_frame_size({640, 480}); capture.set_desktop_geometry_revision(1);
  capture.set_send_message_callback([](const auto&, QString*) { return true; });
  capture.set_local_suspension(redclaw::ui::LocalInputSuspensionReason::kWorkspaceTransfer, true);
  QString error; ASSERT_TRUE(capture.activate(&error)) << error.toStdString();
  EXPECT_TRUE(capture.control_enabled()); EXPECT_FALSE(capture.input_forwarding());
  EXPECT_TRUE(capture.local_suspension_reason().contains("workspace_transfer_busy"));
  capture.pause(false, "transfer grant fixture complete");
}

TEST(ControllerRemoteInputCapture, ReadsLoopbackMarkerFromWindowsMessageQueue) {
  constexpr auto marker = redclaw::input::kRedClawInputExtraInfo;
  const LPARAM previous = SetMessageExtraInfo(static_cast<LPARAM>(marker));
  const auto observed = static_cast<std::uintptr_t>(GetMessageExtraInfo());
  (void)SetMessageExtraInfo(previous);

  EXPECT_EQ(observed, marker);
  EXPECT_TRUE(redclaw::ui::ControllerRemoteInputCapture::should_suppress_mouse_loopback(
      QEvent::MouseMove, observed));
}
#endif

TEST(StunServerPolicy, AutomaticUsesBoundedIndependentProviderPool) {
  const QStringList servers = redclaw::ui::resolve_ice_server_selection(
      redclaw::ui::default_stun_server_preset_id());
  EXPECT_EQ(servers.size(), 3);
  EXPECT_EQ(servers[0], "stun:stun.douyucdn.cn:18000");
  EXPECT_EQ(servers[1], "stun:stun.l.google.com:19302");
  EXPECT_EQ(servers[2], "stun:stun.cloudflare.com:3478");
}

TEST(StunServerPolicy, StrongDomesticCandidatesAreAvailableAsSingleProviders) {
  const auto& presets = redclaw::ui::stun_server_presets();
  const auto expect_single_provider = [&presets](const QString& id, const QString& uri) {
    const auto provider = std::find_if(presets.begin(), presets.end(), [&id](const auto& preset) {
      return preset.id == id;
    });
    ASSERT_NE(provider, presets.end());
    EXPECT_EQ(provider->server_uris, QStringList({uri}));
  };
  expect_single_provider("douyu", "stun:stun.douyucdn.cn:18000");
  expect_single_provider("hitv", "stun:stun.hitv.com:3478");
  expect_single_provider("bilibili", "stun:stun.chat.bilibili.com:3478");
  expect_single_provider("miwifi", "stun:stun.miwifi.com:3478");

  const auto tencent = std::find_if(presets.begin(), presets.end(), [](const auto& preset) {
    return preset.id == "tencent";
  });
  EXPECT_EQ(tencent, presets.end());

  for (const auto& preset : presets) {
    for (const QString& uri : preset.server_uris) {
      EXPECT_NE(uri, "stun:stun.qq.com:3478");
      EXPECT_FALSE(uri.contains("aliyun", Qt::CaseInsensitive));
    }
  }
}

TEST(StunServerPolicy, CustomEntriesAreTrimmedDeduplicatedAndPreserveTurn) {
  const QStringList servers = redclaw::ui::resolve_ice_server_selection(
      u"google",
      {
          " stun:stun.l.google.com:19302 ",
          "# local comment",
          "turn:user:password@relay.example.com:3478",
      });
  ASSERT_EQ(servers.size(), 2);
  EXPECT_EQ(servers[0], "stun:stun.l.google.com:19302");
  EXPECT_EQ(servers[1], "turn:user:password@relay.example.com:3478");
  EXPECT_EQ(redclaw::ui::match_stun_server_preset(servers),
            redclaw::ui::custom_stun_server_preset_id());
}

TEST(ConnectionEntryPage, FitsSupportedWindowSizesAndKeepsInputFocusExplicit) {
  redclaw::ui::ConnectionEntryPage page;
  page.set_local_code("AB12CD34");
  page.set_peer_code("EF56GH78");

  for (const QSize size : {QSize(900, 680), QSize(800, 600)}) {
    page.resize(size);
    page.show();
    QApplication::processEvents();
    EXPECT_LE(page.minimumSizeHint().width(), size.width());
    EXPECT_LE(page.minimumSizeHint().height(), size.height());
  }

  page.peer_code_input()->setFocus(Qt::TabFocusReason);
  QApplication::processEvents();
  EXPECT_TRUE(page.peer_code_input()->hasFocus());
  EXPECT_EQ(page.local_code_display()->focusPolicy(), Qt::NoFocus);

  page.set_actions_enabled(false);
  EXPECT_FALSE(page.peer_code_input()->isEnabled());
  EXPECT_FALSE(page.wait_button()->isEnabled());
  EXPECT_FALSE(page.connect_button()->isEnabled());
  EXPECT_FALSE(page.allow_remote_control_checkbox()->isEnabled());
  EXPECT_FALSE(page.allow_remote_agent_checkbox()->isEnabled());
  EXPECT_FALSE(page.allow_controller_agent_checkbox()->isEnabled());
}

TEST(ConnectionEntryPage, RemoteControlAuthorizationDefaultsFailClosed) {
  redclaw::ui::ConnectionEntryPage page;
  EXPECT_FALSE(page.allow_remote_control_checkbox()->isChecked());
  page.allow_remote_control_checkbox()->setChecked(true);
  EXPECT_TRUE(page.allow_remote_control_checkbox()->isChecked());
}

TEST(ConnectionEntryPage, RemoteAgentAuthorizationDefaultsFailClosed) {
  redclaw::ui::ConnectionEntryPage page;
  EXPECT_FALSE(page.allow_remote_agent_checkbox()->isChecked());
  page.allow_remote_agent_checkbox()->setChecked(true);
  EXPECT_TRUE(page.allow_remote_agent_checkbox()->isChecked());
  EXPECT_FALSE(page.allow_controller_agent_checkbox()->isChecked());
  page.allow_controller_agent_checkbox()->setChecked(true);
  page.allow_remote_agent_checkbox()->setChecked(false);
  EXPECT_TRUE(page.allow_controller_agent_checkbox()->isChecked());
}

TEST(ConnectionProgressPage, ShowsFourExplicitStatesAndActionCallbacks) {
  ConnectionFlowModel model;
  model.start(ConnectionFlowRole::kController);
  model.apply(ConnectionFlowEvent::kRemoteDescriptionApplied);
  model.apply(ConnectionFlowEvent::kFatalFailure, "No waiting device found");

  redclaw::ui::ConnectionProgressPage page(ConnectionFlowRole::kController);
  page.set_connection_code("AB12CD34");
  page.set_snapshot(model.snapshot());
  EXPECT_EQ(page.stage_count(), 4);
  EXPECT_EQ(page.findChildren<QLabel*>("connectionStageArrow").size(), 3);
  EXPECT_EQ(page.stage_state(0), ConnectionStageState::kComplete);
  EXPECT_EQ(page.stage_state(1), ConnectionStageState::kFailed);
  EXPECT_TRUE(page.phase_text().contains("Stage 2 of 4"));
  EXPECT_FALSE(page.retry_button()->isHidden());

  int retries = 0;
  int exits = 0;
  page.set_on_retry([&retries]() { ++retries; });
  page.set_on_exit([&exits]() { ++exits; });
  page.retry_button()->click();
  page.exit_button()->click();
  EXPECT_EQ(retries, 1);
  EXPECT_EQ(exits, 1);
}

TEST(ConnectionEntryPage, RepeatedStatusDoesNotInvalidateTheCurrentStyle) {
  redclaw::ui::ConnectionEntryPage page;
  page.resize(640, 700); page.show(); QCoreApplication::processEvents();
  page.set_status("Connected", "good");
  auto* label = page.findChild<QLabel*>("statusBanner");
  ASSERT_NE(label, nullptr);
  class StyleCounter final : public QObject {
  public:
    int changes = 0;
    bool eventFilter(QObject*, QEvent* event) override {
      if (event->type() == QEvent::StyleChange) ++changes;
      return false;
    }
  } counter;
  label->installEventFilter(&counter);
  for (int i = 0; i < 4; ++i) page.set_status("Connected", "good");
  EXPECT_EQ(counter.changes, 0);
  page.set_status("Reconnecting", "warn");
  EXPECT_GT(counter.changes, 0);
  EXPECT_EQ(label->text(), "Reconnecting");
}

TEST(ConnectionProgressPage, RepeatedSnapshotDoesNotRepolishStageSubtrees) {
  redclaw::ui::ConnectionProgressPage page(ConnectionFlowRole::kController);
  page.resize(820, 620); page.show(); QCoreApplication::processEvents();
  redclaw::ui::ConnectionFlowSnapshot snapshot;
  snapshot.status = ConnectionFlowStatus::kComplete;
  snapshot.stages.fill(ConnectionStageState::kComplete);
  page.set_snapshot(snapshot);
  class StyleCounter final : public QObject {
  public:
    int changes = 0;
    bool eventFilter(QObject*, QEvent* event) override {
      if (event->type() == QEvent::StyleChange) ++changes;
      return false;
    }
  } counter;
  for (auto* child : page.findChildren<QWidget*>()) child->installEventFilter(&counter);
  for (int i = 0; i < 4; ++i) page.set_snapshot(snapshot);
  EXPECT_EQ(counter.changes, 0);
  snapshot.stages[3] = ConnectionStageState::kFailed;
  page.set_snapshot(snapshot);
  EXPECT_GT(counter.changes, 0);
  EXPECT_EQ(page.stage_state(3), ConnectionStageState::kFailed);
}

TEST(ConnectionProgressPage, InitialNetworkFailureKeepsAutomaticRetryWithoutManualAction) {
  ConnectionFlowModel model;
  model.start(ConnectionFlowRole::kController);
  redclaw::ui::ConnectionProgressPage page(ConnectionFlowRole::kController);
  for (int attempt = 1; attempt <= 30; ++attempt) {
    model.apply(ConnectionFlowEvent::kRemoteDescriptionApplied);
    const auto waiting = redclaw::ui::classify_connection_flow_line(
        u"Runtime ICE state failed; DHT retry remains active role=controller",
        ConnectionFlowRole::kController);
    ASSERT_TRUE(waiting.matched);
    model.apply(waiting.event);
    model.apply(waiting.event);  // A repeated observation is not another attempt.
    page.set_snapshot(model.snapshot());
    EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kRetryWaiting);
    EXPECT_EQ(model.snapshot().attempt, attempt);
    EXPECT_TRUE(page.phase_text().contains("retrying automatically"));
    EXPECT_TRUE(page.retry_button()->isHidden());
    EXPECT_FALSE(page.retry_button()->isEnabled());
    EXPECT_TRUE(page.exit_button()->isEnabled());
    const auto retry = redclaw::ui::classify_connection_flow_line(
        u"Runtime rebuilding signaling session role=controller attempt=next",
        ConnectionFlowRole::kController);
    ASSERT_TRUE(retry.matched);
    model.apply(retry.event);
  }
  model.observe_transport(true, true);
  model.apply(ConnectionFlowEvent::kFirstFrameReady);
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kComplete);
  EXPECT_FALSE(model.snapshot().can_retry);

  model.begin_stopping();
  model.apply(ConnectionFlowEvent::kRetryWaiting);
  model.apply(ConnectionFlowEvent::kAutomaticRetry);
  page.set_snapshot(model.snapshot());
  EXPECT_EQ(model.snapshot().status, ConnectionFlowStatus::kStopping);
  EXPECT_TRUE(page.retry_button()->isHidden());
}

TEST(ConnectionProgressPage, BothRolesFitMinimumSupportedWindow) {
  ConnectionFlowModel controller_model;
  controller_model.start(ConnectionFlowRole::kController);
  ConnectionFlowModel host_model;
  host_model.start(ConnectionFlowRole::kHost);

  redclaw::ui::ConnectionProgressPage controller(ConnectionFlowRole::kController);
  redclaw::ui::ConnectionProgressPage host(ConnectionFlowRole::kHost);
  for (auto* page : {&controller, &host}) {
    page->set_connection_code("AB12CD34");
    page->set_snapshot(page == &controller ? controller_model.snapshot() : host_model.snapshot());
    page->resize(800, 600);
    page->show();
    QApplication::processEvents();
    EXPECT_EQ(page->stage_count(), 4);
    EXPECT_EQ(page->findChildren<QLabel*>("connectionStageArrow").size(), 3);
    EXPECT_LE(page->minimumSizeHint().width(), 800);
    EXPECT_LE(page->minimumSizeHint().height(), 600);
  }
}

}  // namespace


#include "window_stall_dump.h"
class TimedTestApplication final : public QApplication {
public:
  using QApplication::QApplication;
  qint64 paint_us = 0;
  qint64 paint_count = 0;
  WindowStallDump stall_dump;
  static qint64 thread_cpu_us() {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) {
      return static_cast<qint64>(((static_cast<std::uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime)
          + ((static_cast<std::uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime)) / 10;
    }
#endif
    return 0;
  }
  bool notify(QObject* receiver, QEvent* event) override {
    if (!qEnvironmentVariableIsSet("REDCLAW_UI_EVENT_PROBE"))
      return QApplication::notify(receiver, event);
    const int type = static_cast<int>(event->type());
    const QByteArray name = receiver->metaObject()->className();
    const auto paint_before = paint_us;
    const auto count_before = paint_count;
    const auto cpu_before = thread_cpu_us();
    QElapsedTimer elapsed;
    elapsed.start();
    if (type == QEvent::UpdateRequest) stall_dump.begin();
    const bool result = QApplication::notify(receiver, event);
    if (type == QEvent::UpdateRequest) stall_dump.end();
    if (type == 12) { paint_us += elapsed.nsecsElapsed() / 1000; ++paint_count; }
    if (elapsed.elapsed() > 100)
      std::fprintf(stderr, "UI event type=%d class=%s duration_ms=%lld cpu_ms=%lld paint_ms=%lld paint_count=%lld\n",
          type, name.constData(), elapsed.elapsed(), (thread_cpu_us() - cpu_before) / 1000,
          (paint_us - paint_before) / 1000, paint_count - count_before);
    return result;
  }
};

int run_runtime_stdio_fixture(const char* mode);
int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--stdio-fixture") return run_runtime_stdio_fixture(argv[2]);
  TimedTestApplication application(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
