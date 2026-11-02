#include <gtest/gtest.h>

#include <QApplication>
#include <QButtonGroup>
#include <QLabel>
#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFrame>
#include <QMouseEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTimer>

#include <algorithm>
#include <vector>

#include "ui/desktop_navigation_panel.h"
#include "ui/desktop_task_workspace.h"
#include "redclaw/protocol/capture_region_control.h"

namespace {

std::vector<redclaw::protocol::DesktopDisplayV1> displays() {
  return {
      {
          .display_id = "display-a",
          .display_name = "Primary",
          .desktop_origin_x = 0,
          .desktop_origin_y = 0,
          .pixel_width = 1920,
          .pixel_height = 1080,
          .rotation = 0,
          .primary = true,
      },
      {
          .display_id = "display-b",
          .display_name = "Portrait",
          .desktop_origin_x = -1080,
          .desktop_origin_y = 0,
          .pixel_width = 1080,
          .pixel_height = 1920,
          .rotation = 90,
          .primary = false,
      },
  };
}

redclaw::protocol::StreamControlMessageV1 region_message(
    redclaw::protocol::StreamControlMessageTypeV1 type,
    const std::string& display_id,
    std::uint64_t revision) {
  redclaw::protocol::StreamControlMessageV1 message;
  message.type = type;
  message.display_id = display_id;
  message.region_right = 65535;
  message.region_bottom = 65535;
  message.capture_region_revision = revision;
  return message;
}

TEST(DesktopNavigationPanelTests, SwitchFailureReturnsToConfirmedDisplay) {
  redclaw::ui::DesktopNavigationPanel panel;
  panel.set_display_catalog(displays(), 1);
  panel.set_transport_available(true);

  std::string requested_display;
  std::uint64_t requested_revision = 0;
  panel.set_region_request_callback(
      [&](const std::string& display_id,
          std::uint16_t,
          std::uint16_t,
          std::uint16_t,
          std::uint16_t,
          std::uint64_t revision) {
        requested_display = display_id;
        requested_revision = revision;
        return true;
      });
  auto* picker = panel.findChild<QButtonGroup*>();
  ASSERT_NE(picker, nullptr);
  picker->button(1)->click();
  EXPECT_EQ(requested_display, "display-b");
  EXPECT_EQ(panel.selected_display_id(), QString("display-b"));

  auto rejected = region_message(
      redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionRejected,
      "display-b",
      requested_revision);
  rejected.payload = "capture_switch_failed";
  panel.apply_region_rejected(rejected);
  EXPECT_EQ(panel.selected_display_id(), QString("display-a"));
  EXPECT_EQ(panel.pending_region_revision(), 0U);
}

TEST(DesktopNavigationPanelTests, DisplayCardsUseHumanLabelsAndPreserveIdentityWithoutDriverNames) {
  redclaw::ui::DesktopNavigationPanel panel;
  panel.resize(520, 440);
  auto catalog = displays();
  catalog[0].display_name = "NVIDIA GeForce driver adapter";
  catalog[1].display_name = "Microsoft Remote Display Adapter";
  panel.set_display_catalog(catalog, 1);
  panel.set_transport_available(true);
  QFile style(qEnvironmentVariable("REDCLAW_UI_STYLE_FILE"));
  if (style.open(QIODevice::ReadOnly)) panel.setStyleSheet(QString::fromUtf8(style.readAll()));
  panel.show(); QApplication::processEvents();
  auto* picker = panel.findChild<QButtonGroup*>();
  ASSERT_NE(picker, nullptr); ASSERT_EQ(picker->buttons().size(), 2);
  EXPECT_EQ(picker->checkedId(), 0);
  EXPECT_TRUE(picker->button(0)->text().contains(QString::fromUtf8("显示器 1 · 主屏")));
  EXPECT_TRUE(picker->button(1)->text().contains(QString::fromUtf8("显示器 2")));
  EXPECT_TRUE(picker->button(1)->accessibleName().contains(QString::fromUtf8("1080 × 1920")));
  EXPECT_TRUE(picker->button(1)->toolTip().contains(QString::fromUtf8("纵向")));
  for (auto* button : picker->buttons()) {
    EXPECT_FALSE(button->text().contains("Adapter")); EXPECT_FALSE(button->toolTip().contains("NVIDIA"));
    EXPECT_FALSE(button->accessibleName().isEmpty());
  }
  int requests = 0;
  panel.set_region_request_callback([&](const std::string& id, auto, auto, auto, auto, auto) {
    EXPECT_EQ(id, "display-b"); ++requests; return true;
  });
  const auto output = qEnvironmentVariable("REDCLAW_UI_EVIDENCE_DIR");
  if (!output.isEmpty()) { QDir().mkpath(output); panel.grab().save(output + "/display-picker.png"); }
  picker->button(1)->click(); picker->button(1)->click();
  EXPECT_EQ(requests, 1); EXPECT_EQ(panel.selected_display_id(), QString("display-b"));
  std::reverse(catalog.begin(), catalog.end());
  panel.set_display_catalog(catalog, 2);
  EXPECT_EQ(panel.selected_display_id(), QString("display-b")); EXPECT_EQ(picker->checkedId(), 0);
}

TEST(DesktopNavigationPanelTests, DisplayCardsRespectConnectionAndTransferGatesWithoutIssuingRequests) {
  redclaw::ui::DesktopNavigationPanel panel;
  panel.set_display_catalog(displays(), 1);
  auto* picker = panel.findChild<QButtonGroup*>(); ASSERT_NE(picker, nullptr);
  int requests = 0;
  panel.set_region_request_callback([&](const std::string&, auto, auto, auto, auto, auto) { ++requests; return true; });
  EXPECT_FALSE(picker->button(1)->isEnabled()); picker->button(1)->click();
  EXPECT_EQ(requests, 0);
  panel.set_transport_available(true); panel.set_workspace_blocked(true);
  EXPECT_FALSE(picker->button(1)->isEnabled()); picker->button(1)->click();
  EXPECT_EQ(requests, 0);
  panel.set_workspace_blocked(false); EXPECT_TRUE(picker->button(1)->isEnabled());
  picker->button(1)->click(); EXPECT_EQ(requests, 1);
  panel.set_transport_available(false); EXPECT_FALSE(picker->button(0)->isEnabled());
}

void send_button_mouse_click(QAbstractButton* button) {
  const QPointF position = button->rect().center();
  const QPointF global = button->mapToGlobal(position.toPoint());
  QMouseEvent press(QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(button, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(button, &release);
  QApplication::processEvents();
}

TEST(DesktopNavigationPanelTests, FirstClickOnDefaultScreenMustRequestHostBeforeItCanBeTreatedAsApplied) {
  redclaw::ui::DesktopNavigationPanel panel;
  panel.resize(360, 274); panel.set_display_catalog(displays(), 1, 1);
  panel.set_transport_available(true); panel.show(); QApplication::processEvents();
  auto* picker = panel.findChild<QButtonGroup*>(); ASSERT_NE(picker, nullptr);
  int requests = 0;
  std::uint64_t token = 0;
  panel.set_region_request_callback([&](const std::string& id, auto, auto, auto, auto, auto request) {
    EXPECT_EQ(id, "display-a"); ++requests; token = request; return true;
  });
  send_button_mouse_click(picker->button(0));
  ASSERT_EQ(requests, 1);
  EXPECT_EQ(panel.pending_region_revision(), token);
  send_button_mouse_click(picker->button(0)); EXPECT_EQ(requests, 1);
  auto reply = region_message(redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionApplied, "display-a", 900);
  reply.capture_region_command_version = 1; reply.capture_region_request_id = token;
  panel.apply_region_applied(reply);
  send_button_mouse_click(picker->button(0)); EXPECT_EQ(requests, 1);
  panel.set_transport_available(false); panel.set_display_catalog(displays(), 1, 1); panel.set_transport_available(true);
  send_button_mouse_click(picker->button(0)); EXPECT_EQ(requests, 2);
}

TEST(DesktopNavigationPanelTests, DefaultFloatingNavigationLeavesPreviewVisibleAndMouseClicksReachAnotherScreen) {
  QWidget owner; owner.resize(1000, 700); owner.show();
  redclaw::ui::DesktopTaskWorkspace workspace(&owner, nullptr);
  auto* panel = new redclaw::ui::DesktopNavigationPanel;
  panel->set_display_catalog(displays(), 1, 1); panel->set_transport_available(true);
  workspace.add_task(redclaw::ui::DesktopTask::kNavigation, QString::fromUtf8("桌面导航"), panel, QSize(360, 320));
  workspace.set_task_visible(redclaw::ui::DesktopTask::kNavigation, true);
  QFile style(qEnvironmentVariable("REDCLAW_UI_STYLE_FILE"));
  if (!style.fileName().isEmpty() && style.open(QIODevice::ReadOnly)) workspace.task_window(redclaw::ui::DesktopTask::kNavigation)->setStyleSheet(QString::fromUtf8(style.readAll()));
  QApplication::processEvents();
  auto* selection = panel->findChild<QWidget*>("desktopNavigationSelection"); ASSERT_NE(selection, nullptr);
  EXPECT_GE(selection->visibleRegion().boundingRect().height(), 120);
  auto* picker = panel->findChild<QButtonGroup*>(); ASSERT_NE(picker, nullptr);
  EXPECT_LE(picker->button(0)->height(), 42);
  int requests = 0;
  panel->set_region_request_callback([&](const std::string& id, auto, auto, auto, auto, auto) {
    EXPECT_EQ(id, "display-b"); ++requests; return true;
  });
  send_button_mouse_click(picker->button(1)); EXPECT_EQ(requests, 1);
  EXPECT_EQ(panel->selected_display_id(), QString("display-b"));
  auto reply = region_message(redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionApplied, "display-b", 900);
  reply.capture_region_command_version = 1; reply.capture_region_request_id = panel->pending_region_revision();
  panel->apply_region_applied(reply);
  EXPECT_EQ(panel->pending_region_revision(), 0U);
  EXPECT_EQ(panel->confirmed_display_id(), QString("display-b"));
  const auto output = qEnvironmentVariable("REDCLAW_UI_EVIDENCE_DIR");
  if (!output.isEmpty()) workspace.task_window(redclaw::ui::DesktopTask::kNavigation)->grab().save(output + "/navigation-360x320.png");
}

TEST(DesktopNavigationPanelTests, SingleDisplayHintDoesNotPromiseAnotherScreen) {
  redclaw::ui::DesktopNavigationPanel panel;
  auto catalog = displays(); catalog.resize(1);
  panel.set_display_catalog(catalog, 1, 1); panel.set_transport_available(true);
  auto* status = panel.findChild<QLabel*>("desktopNavigationStatus"); ASSERT_NE(status, nullptr);
  EXPECT_TRUE(status->text().contains(QString::fromUtf8("1 个显示器")));
  EXPECT_FALSE(status->text().contains(QString::fromUtf8("切换")));
  EXPECT_EQ(panel.diagnostic_snapshot().value("display_count").toInt(), 1);
  EXPECT_FALSE(panel.diagnostic_snapshot().value("host_confirmation_known").toBool());
}

TEST(DesktopNavigationPanelTests, NegotiatedCommandsDoNotChaseHostGeometryOrAcceptOldReplies) {
  using namespace redclaw::protocol;
  redclaw::ui::DesktopNavigationPanel panel;
  panel.set_display_catalog(displays(), 1, kCaptureRegionCommandVersion);
  panel.set_transport_available(true);
  panel.observe_capture_region_revision(900);
  std::vector<std::uint64_t> requests;
  panel.set_region_request_callback([&](const std::string&, auto, auto, auto, auto, auto id) {
    requests.push_back(id); return true;
  });
  auto* picker = panel.findChild<QButtonGroup*>();
  ASSERT_NE(picker, nullptr);
  picker->button(1)->click();
  ASSERT_EQ(requests, (std::vector<std::uint64_t>{2}));
  auto reply = region_message(StreamControlMessageTypeV1::kCaptureRegionApplied, "display-b", 901);
  reply.capture_region_command_version = 1;
  reply.capture_region_request_id = 1;
  panel.apply_region_applied(reply);
  EXPECT_EQ(panel.pending_region_revision(), 2U);
  reply.capture_region_request_id = 2;
  panel.apply_region_applied(reply);
  EXPECT_EQ(panel.pending_region_revision(), 0U);
  EXPECT_EQ(panel.confirmed_display_id(), QString("display-b"));
  picker->button(0)->click();
  panel.apply_region_applied(reply);
  EXPECT_EQ(panel.pending_region_revision(), 3U);
  EXPECT_EQ(requests, (std::vector<std::uint64_t>{2, 3}));
  reply.type = StreamControlMessageTypeV1::kCaptureRegionRejected;
  reply.capture_region_revision = 0;
  reply.capture_region_request_id = 3;
  reply.payload = "display_unavailable";
  panel.apply_region_rejected(reply);
  EXPECT_EQ(panel.pending_region_revision(), 0U);
  EXPECT_EQ(panel.selected_display_id(), QString("display-b"));
}

TEST(DesktopNavigationPanelTests, ReconnectRenegotiatesAfterHostUpgradeOrDowngrade) {
  redclaw::ui::DesktopNavigationPanel panel;
  panel.set_display_catalog(displays(), 1, 1);
  panel.set_transport_available(true);
  panel.set_transport_available(false);
  panel.set_display_catalog(displays(), 1); // Replacement Host is legacy.
  panel.set_transport_available(true);
  panel.observe_capture_region_revision(7);
  std::uint64_t request_id = 0;
  panel.set_region_request_callback([&](const std::string&, auto, auto, auto, auto, auto id) {
    request_id = id; return true;
  });
  auto* picker = panel.findChild<QButtonGroup*>();
  ASSERT_NE(picker, nullptr);
  picker->button(1)->click();
  EXPECT_EQ(panel.capture_region_command_version(), 0U);
  EXPECT_EQ(request_id, 8U);
  panel.set_transport_available(false);
  panel.set_display_catalog(displays(), 1, 1);
  panel.set_transport_available(true);
  panel.observe_capture_region_revision(900);
  picker->button(0)->click();
  EXPECT_EQ(panel.capture_region_command_version(), 1U);
  EXPECT_EQ(request_id, 2U);
}

TEST(DesktopNavigationPanelTests, HostRevisionOnAnotherDisplayAdvancesNextRequest) {
  redclaw::ui::DesktopNavigationPanel panel;
  panel.set_display_catalog(displays(), 1);
  panel.set_transport_available(true);
  panel.apply_region_applied(region_message(
      redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionApplied,
      "display-b", 11));
  EXPECT_EQ(panel.selected_display_id(), QString("display-a"));
  std::uint64_t requested_revision = 0;
  panel.set_region_request_callback(
      [&](const std::string&, std::uint16_t, std::uint16_t,
          std::uint16_t, std::uint16_t, std::uint64_t revision) {
        requested_revision = revision;
        return true;
      });
  auto* picker = panel.findChild<QButtonGroup*>();
  ASSERT_NE(picker, nullptr);
  picker->button(1)->click();
  EXPECT_EQ(requested_revision, 12U);
}

TEST(DesktopNavigationPanelTests, SelectionCommitsOnlyOnMouseReleaseAndIsRemembered) {
  redclaw::ui::DesktopNavigationPanel panel;
  panel.resize(520, 360);
  panel.set_display_catalog(displays(), 1);
  panel.set_transport_available(true);
  QImage thumbnail(320, 180, QImage::Format_RGB32);
  thumbnail.fill(Qt::darkBlue);
  panel.set_thumbnail("display-a", thumbnail, 1);
  panel.show();
  QApplication::processEvents();

  int request_count = 0;
  std::string last_display;
  std::uint16_t last_right = 0;
  panel.set_region_request_callback(
      [&](const std::string& display_id,
          std::uint16_t,
          std::uint16_t,
          std::uint16_t right,
          std::uint16_t,
          std::uint64_t) {
        ++request_count;
        last_display = display_id;
        last_right = right;
        return true;
      });

  auto* selection = panel.findChild<QWidget*>("desktopNavigationSelection");
  ASSERT_NE(selection, nullptr);
  const qreal scale = std::min(
      static_cast<qreal>(selection->width()) / 320.0,
      static_cast<qreal>(selection->height() - 24) / 180.0);
  const qreal content_width = 320.0 * scale;
  const qreal content_height = 180.0 * scale;
  const qreal content_left = (selection->width() - content_width) / 2.0;
  const qreal content_top = 12.0 + (selection->height() - 24.0 - content_height) / 2.0;
  const QPointF start(
      content_left + content_width - 1.0,
      content_top + content_height / 2.0);
  const QPointF finish(content_left + content_width * 0.75, start.y());
  QMouseEvent press(
      QEvent::MouseButtonPress, start, start, start,
      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(selection, &press);
  QMouseEvent move(
      QEvent::MouseMove, finish, finish, finish,
      Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(selection, &move);
  EXPECT_EQ(request_count, 0);
  QMouseEvent release(
      QEvent::MouseButtonRelease, finish, finish, finish,
      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(selection, &release);
  ASSERT_EQ(request_count, 1);
  EXPECT_EQ(last_display, "display-a");
  EXPECT_LT(last_right, 65535U);
  const std::uint16_t remembered_right = last_right;

  auto* picker = panel.findChild<QButtonGroup*>();
  ASSERT_NE(picker, nullptr);
  picker->button(1)->click();
  EXPECT_EQ(last_display, "display-b");
  EXPECT_EQ(last_right, 65535U);
  picker->button(0)->click();
  EXPECT_EQ(last_display, "display-a");
  EXPECT_EQ(last_right, remembered_right);
}

TEST(DesktopNavigationPanelTests, ObservedHostRevisionSurvivesCatalogRefreshAndPendingRequest) {
  redclaw::ui::DesktopNavigationPanel panel;
  panel.set_display_catalog(displays(), 1);
  panel.set_transport_available(true);
  panel.observe_capture_region_revision(11);
  panel.observe_capture_region_revision(0);
  panel.observe_capture_region_revision(3);
  panel.set_display_catalog(displays(), 1);
  std::uint64_t requested_revision = 0;
  panel.set_region_request_callback(
      [&](const std::string&, std::uint16_t, std::uint16_t,
          std::uint16_t, std::uint16_t, std::uint64_t revision) {
        requested_revision = revision;
        return true;
      });
  auto* picker = panel.findChild<QButtonGroup*>();
  ASSERT_NE(picker, nullptr);
  picker->button(1)->click();
  EXPECT_EQ(requested_revision, 12U);
  panel.observe_capture_region_revision(11);
  panel.observe_capture_region_revision(12);
  EXPECT_EQ(panel.pending_region_revision(), 12U);
  EXPECT_EQ(panel.confirmed_display_id(), QString("display-a"));
  EXPECT_EQ(panel.selected_display_id(), QString("display-b"));
  panel.apply_region_applied(region_message(
      redclaw::protocol::StreamControlMessageTypeV1::kCaptureRegionApplied,
      "display-b", 12));
  EXPECT_EQ(panel.pending_region_revision(), 0U);
  EXPECT_EQ(panel.confirmed_display_id(), QString("display-b"));
  picker->button(0)->click();
  EXPECT_EQ(requested_revision, 13U);
}

TEST(DesktopNavigationPanelTests, NewHostSessionResetsObservedRegionRevision) {
  redclaw::ui::DesktopNavigationPanel panel;
  panel.set_display_catalog(displays(), 1);
  panel.set_transport_available(true);
  panel.observe_capture_region_revision(900);
  panel.set_transport_available(false);
  panel.set_display_catalog(displays(), 1);
  panel.set_transport_available(true);
  panel.observe_capture_region_revision(1);
  std::uint64_t requested_revision = 0;
  panel.set_region_request_callback(
      [&](const std::string&, std::uint16_t, std::uint16_t,
          std::uint16_t, std::uint16_t, std::uint64_t revision) {
        requested_revision = revision;
        return true;
      });
  auto* picker = panel.findChild<QButtonGroup*>();
  ASSERT_NE(picker, nullptr);
  picker->button(1)->click();
  EXPECT_EQ(requested_revision, 2U);
}

TEST(DesktopNavigationPanelTests, IndependentNavigationFailedSendRollsBack) {
  redclaw::ui::DesktopNavigationPanel navigation;
  auto* panel = &navigation;
  panel->set_display_catalog(displays(), 1);
  panel->set_transport_available(true);
  panel->set_region_request_callback(
      [](const std::string&, std::uint16_t, std::uint16_t,
         std::uint16_t, std::uint16_t, std::uint64_t) { return false; });
  auto* picker = panel->findChild<QButtonGroup*>();
  ASSERT_NE(picker, nullptr);
  picker->button(1)->click();
  EXPECT_EQ(panel->selected_display_id(), QString("display-a"));
  EXPECT_EQ(panel->pending_region_revision(), 0U);
}

TEST(DesktopNavigationPanelTests, ReconnectAcceptsNewHostRevisionsForEveryDisplay) {
  redclaw::ui::DesktopNavigationPanel panel;
  panel.resize(520, 360);
  panel.set_display_catalog(displays(), 90);
  panel.set_transport_available(true);
  panel.show();
  QApplication::processEvents();
  auto* picker = panel.findChild<QButtonGroup*>();
  auto* selection = panel.findChild<QWidget*>("desktopNavigationSelection");
  ASSERT_NE(picker, nullptr);
  ASSERT_NE(selection, nullptr);
  QImage thumbnail(320, 180, QImage::Format_RGB32);
  const auto color_at_center = [&] {
    const QImage rendered = selection->grab().toImage();
    return rendered.pixelColor(rendered.width() / 2, rendered.height() / 2);
  };
  thumbnail.fill(Qt::red);
  panel.set_thumbnail("display-a", thumbnail, 900);
  panel.set_thumbnail("display-b", thumbnail, 901);
  picker->button(1)->click();
  const QColor old_color = color_at_center();
  thumbnail.fill(Qt::green);
  panel.set_thumbnail("display-b", thumbnail, 1);
  EXPECT_EQ(color_at_center(), old_color); // Keep rejecting stale frames in one session.

  panel.set_transport_available(false);
  EXPECT_NE(color_at_center(), old_color);
  auto new_catalog = displays();
  new_catalog[1].display_name = "New Host DISPLAY2";
  panel.set_display_catalog(new_catalog, 1);
  panel.set_transport_available(true);
  EXPECT_EQ(picker->checkedId(), 1);
  EXPECT_FALSE(picker->button(1)->text().contains("New Host DISPLAY2"));
  EXPECT_EQ(panel.selected_display_id(), QString("display-b"));
  for (int index = 0; index < 2; ++index) {
    picker->button(index)->click();
    const auto id = panel.selected_display_id().toStdString();
    thumbnail.fill(Qt::green);
    panel.set_thumbnail(id, thumbnail, 1);
    const QColor first = color_at_center();
    EXPECT_GT(first.green(), first.red());
    thumbnail.fill(Qt::blue);
    panel.set_thumbnail(id, thumbnail, 2);
    const QColor second = color_at_center();
    EXPECT_GT(second.blue(), second.green());
    thumbnail.fill(Qt::red);
    panel.set_thumbnail(id, thumbnail, 1);
    EXPECT_EQ(color_at_center(), second);
  }
}

}  // namespace
