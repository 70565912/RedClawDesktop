#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QSettings>
#include <QTemporaryDir>
#include <QToolButton>

#include <algorithm>
#include <vector>

#include "ui/desktop_navigation_panel.h"

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
  auto* combo = panel.findChild<QComboBox*>();
  ASSERT_NE(combo, nullptr);
  combo->setCurrentIndex(1);
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

  auto* combo = panel.findChild<QComboBox*>();
  ASSERT_NE(combo, nullptr);
  combo->setCurrentIndex(1);
  EXPECT_EQ(last_display, "display-b");
  EXPECT_EQ(last_right, 65535U);
  combo->setCurrentIndex(0);
  EXPECT_EQ(last_display, "display-a");
  EXPECT_EQ(last_right, remembered_right);
}

TEST(DesktopNavigationPanelTests, FailedSendRollsBackAndOverlayDoesNotResizeContent) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  QSettings settings(directory.filePath("navigation.ini"), QSettings::IniFormat);
  auto* content = new QWidget();
  content->setMinimumHeight(300);
  redclaw::ui::DesktopNavigationOverlayHost host(content, &settings);
  host.resize(420, 720);
  host.show();
  QApplication::processEvents();

  const int content_height = content->height();
  EXPECT_FALSE(host.expanded());
  host.set_expanded(true);
  QApplication::processEvents();
  EXPECT_TRUE(host.expanded());
  EXPECT_EQ(content->height(), content_height);
  host.set_overlay_height(340);
  EXPECT_EQ(host.overlay_height(), 340);
  EXPECT_EQ(settings.value("controller/navigation_panel_height").toInt(), 340);

  auto* panel = host.navigation_panel();
  panel->set_display_catalog(displays(), 1);
  panel->set_transport_available(true);
  panel->set_region_request_callback(
      [](const std::string&, std::uint16_t, std::uint16_t,
         std::uint16_t, std::uint16_t, std::uint64_t) { return false; });
  auto* combo = panel->findChild<QComboBox*>();
  ASSERT_NE(combo, nullptr);
  combo->setCurrentIndex(1);
  EXPECT_EQ(panel->selected_display_id(), QString("display-a"));
  EXPECT_EQ(panel->pending_region_revision(), 0U);
}

}  // namespace
