#pragma once

#include "redclaw/protocol/stream_control_protocol.h"

#include <QImage>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class QComboBox;
class QFrame;
class QLabel;
class QResizeEvent;
class QSettings;
class QToolButton;

namespace redclaw::ui {

class NavigationSelectionWidget;

class DesktopNavigationPanel final : public QWidget {
 public:
  using RegionRequestCallback = std::function<bool(
      const std::string& display_id,
      std::uint16_t left,
      std::uint16_t top,
      std::uint16_t right,
      std::uint16_t bottom,
      std::uint64_t revision)>;

  explicit DesktopNavigationPanel(QWidget* parent = nullptr);

  void set_region_request_callback(RegionRequestCallback callback);
  void set_display_catalog(
      const std::vector<redclaw::protocol::DesktopDisplayV1>& displays,
      std::uint64_t catalog_revision);
  void set_thumbnail(
      const std::string& display_id,
      const QImage& thumbnail,
      std::uint64_t thumbnail_revision);
  void apply_region_applied(
      const redclaw::protocol::StreamControlMessageV1& message);
  void apply_region_rejected(
      const redclaw::protocol::StreamControlMessageV1& message);
  void set_transport_available(bool available);

  [[nodiscard]] QString selected_display_id() const;
  [[nodiscard]] QString confirmed_display_id() const;
  [[nodiscard]] QRectF selected_normalized_region() const;
  [[nodiscard]] std::uint64_t pending_region_revision() const;
  [[nodiscard]] bool pending_request_changes_display() const;

 private:
  friend class NavigationSelectionWidget;

  void select_display(int index, bool submit);
  void commit_current_selection();
  void update_display_text();
  [[nodiscard]] const redclaw::protocol::DesktopDisplayV1* current_display() const;

  QComboBox* display_combo_ = nullptr;
  QLabel* resolution_label_ = nullptr;
  QLabel* status_label_ = nullptr;
  NavigationSelectionWidget* selection_widget_ = nullptr;
  std::vector<redclaw::protocol::DesktopDisplayV1> displays_;
  std::uint64_t catalog_revision_ = 0;
  std::uint64_t next_region_revision_ = 2;
  std::uint64_t pending_region_revision_ = 0;
  QString confirmed_display_id_;
  bool transport_available_ = false;
  RegionRequestCallback request_callback_;
};

class DesktopNavigationOverlayHost final : public QWidget {
 public:
  explicit DesktopNavigationOverlayHost(
      QWidget* content,
      QSettings* settings,
      QWidget* parent = nullptr);

  [[nodiscard]] DesktopNavigationPanel* navigation_panel() const;
  [[nodiscard]] bool expanded() const;
  [[nodiscard]] int overlay_height() const;
  void set_expanded(bool expanded);
  void set_overlay_height(int height);

 protected:
  void resizeEvent(QResizeEvent* event) override;

 private:
  void update_overlay_geometry();

  QSettings* settings_ = nullptr;
  QToolButton* toggle_ = nullptr;
  QWidget* content_container_ = nullptr;
  QFrame* overlay_ = nullptr;
  DesktopNavigationPanel* navigation_panel_ = nullptr;
  int overlay_height_ = 260;
};

}  // namespace redclaw::ui
