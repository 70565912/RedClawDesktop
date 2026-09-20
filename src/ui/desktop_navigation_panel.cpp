#include "ui/desktop_navigation_panel.h"

#include <QComboBox>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace redclaw::ui {
namespace {

constexpr qreal kHandleRadius = 10.0;
constexpr qreal kVerticalHandleMargin = 12.0;

struct DisplayMemory {
  QRectF region{0.0, 0.0, 1.0, 1.0};
  QRectF confirmed_region{0.0, 0.0, 1.0, 1.0};
  QImage thumbnail;
  std::uint64_t thumbnail_revision = 0;
};

QRectF clamp_region(QRectF region, qreal min_width, qreal min_height) {
  region = region.normalized();
  region.setWidth(std::clamp(region.width(), min_width, 1.0));
  region.setHeight(std::clamp(region.height(), min_height, 1.0));
  region.moveLeft(std::clamp(region.left(), 0.0, 1.0 - region.width()));
  region.moveTop(std::clamp(region.top(), 0.0, 1.0 - region.height()));
  return region;
}

}  // namespace

class NavigationSelectionWidget final : public QWidget {
 public:
  explicit NavigationSelectionWidget(DesktopNavigationPanel* panel)
      : QWidget(panel), panel_(panel) {
    setMinimumHeight(150);
    setObjectName("desktopNavigationSelection");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
  }

  void set_display(QString id, QSize source_size) {
    display_id_ = std::move(id);
    source_size_ = source_size;
    if (!memories_.contains(display_id_)) {
      memories_.insert(display_id_, DisplayMemory{});
    }
    update();
  }

  void set_thumbnail(const QString& id, const QImage& image, std::uint64_t revision) {
    auto& memory = memories_[id];
    if (revision <= memory.thumbnail_revision || image.isNull()) {
      return;
    }
    memory.thumbnail = image.copy();
    memory.thumbnail_revision = revision;
    if (id == display_id_) {
      update();
    }
  }

  [[nodiscard]] QRectF region() const {
    return memories_.value(display_id_).region;
  }

  void set_confirmed_region(const QRectF& region) {
    auto& memory = memories_[display_id_];
    memory.region = region;
    memory.confirmed_region = region;
    update();
  }

  void rollback() {
    auto& memory = memories_[display_id_];
    memory.region = memory.confirmed_region;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(4, 10, 25));
    const auto memory = memories_.value(display_id_);
    if (memory.thumbnail.isNull()) {
      painter.setPen(QColor(148, 163, 184));
      painter.drawText(rect(), Qt::AlignCenter, "Waiting for desktop thumbnail...");
      return;
    }
    const QRectF content = content_rect(memory.thumbnail.size());
    painter.drawImage(content, memory.thumbnail);
    painter.setPen(QPen(QColor(56, 189, 248), 2.0));
    painter.setBrush(QColor(14, 165, 233, 38));
    const QRectF selection = normalized_to_widget(memory.region, content);
    painter.drawRect(selection);
    painter.setBrush(QColor(224, 242, 254));
    for (const QPointF& point : handle_points(selection)) {
      painter.drawRect(QRectF(point.x() - 3, point.y() - 3, 6, 6));
    }
  }

  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() != Qt::LeftButton || display_id_.isEmpty()) {
      return;
    }
    const auto memory = memories_.value(display_id_);
    if (memory.thumbnail.isNull()) {
      return;
    }
    setFocus(Qt::MouseFocusReason);
    drag_content_rect_ = content_rect(memory.thumbnail.size());
    if (!drag_content_rect_.contains(event->position())) {
      return;
    }
    drag_start_ = widget_to_normalized(event->position(), drag_content_rect_);
    drag_region_ = memory.region;
    drag_mode_ = hit_test(event->position(), normalized_to_widget(drag_region_, drag_content_rect_));
    dragging_ = drag_mode_ != DragMode::kNone;
    if (dragging_) {
      grabMouse();
    }
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (!dragging_) {
      return;
    }
    const QPointF current = widget_to_normalized(event->position(), drag_content_rect_);
    const QPointF delta = current - drag_start_;
    QRectF next = drag_region_;
    if (drag_mode_ == DragMode::kMove) {
      next.moveTopLeft(drag_region_.topLeft() + delta);
    } else {
      if (has_left(drag_mode_)) next.setLeft(drag_region_.left() + delta.x());
      if (has_right(drag_mode_)) next.setRight(drag_region_.right() + delta.x());
      if (has_top(drag_mode_)) next.setTop(drag_region_.top() + delta.y());
      if (has_bottom(drag_mode_)) next.setBottom(drag_region_.bottom() + delta.y());
    }
    const qreal min_width = source_size_.width() > 0
        ? std::min(1.0, 64.0 / source_size_.width()) : 1.0;
    const qreal min_height = source_size_.height() > 0
        ? std::min(1.0, 64.0 / source_size_.height()) : 1.0;
    memories_[display_id_].region = clamp_region(next, min_width, min_height);
    update();
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (event->button() != Qt::LeftButton || !dragging_) {
      return;
    }
    dragging_ = false;
    drag_mode_ = DragMode::kNone;
    releaseMouse();
    panel_->commit_current_selection();
  }

 private:
  enum class DragMode {
    kNone, kMove, kLeft, kRight, kTop, kBottom,
    kTopLeft, kTopRight, kBottomLeft, kBottomRight,
  };

  [[nodiscard]] QRectF content_rect(const QSize& image_size) const {
    if (image_size.isEmpty()) return {};
    QRectF bounds(rect());
    bounds.adjust(0.0, kVerticalHandleMargin, 0.0, -kVerticalHandleMargin);
    if (bounds.isEmpty()) return {};
    QSizeF scaled = image_size;
    scaled.scale(bounds.size(), Qt::KeepAspectRatio);
    return QRectF(
        bounds.left() + (bounds.width() - scaled.width()) / 2.0,
        bounds.top() + (bounds.height() - scaled.height()) / 2.0,
        scaled.width(), scaled.height());
  }

  static QRectF normalized_to_widget(const QRectF& region, const QRectF& content) {
    return QRectF(
        content.left() + region.left() * content.width(),
        content.top() + region.top() * content.height(),
        region.width() * content.width(),
        region.height() * content.height());
  }

  static QPointF widget_to_normalized(const QPointF& point, const QRectF& content) {
    return QPointF(
        std::clamp((point.x() - content.left()) / content.width(), 0.0, 1.0),
        std::clamp((point.y() - content.top()) / content.height(), 0.0, 1.0));
  }

  static std::array<QPointF, 8> handle_points(const QRectF& rect) {
    return {rect.topLeft(), QPointF(rect.center().x(), rect.top()), rect.topRight(),
            QPointF(rect.right(), rect.center().y()), rect.bottomRight(),
            QPointF(rect.center().x(), rect.bottom()), rect.bottomLeft(),
            QPointF(rect.left(), rect.center().y())};
  }

  static DragMode hit_test(const QPointF& point, const QRectF& rect) {
    const auto points = handle_points(rect);
    const std::array<DragMode, 8> modes = {
        DragMode::kTopLeft, DragMode::kTop, DragMode::kTopRight,
        DragMode::kRight, DragMode::kBottomRight, DragMode::kBottom,
        DragMode::kBottomLeft, DragMode::kLeft};
    for (std::size_t index = 0; index < points.size(); ++index) {
      if (QLineF(point, points[index]).length() <= kHandleRadius) {
        return modes[index];
      }
    }
    return rect.contains(point) ? DragMode::kMove : DragMode::kNone;
  }

  static bool has_left(DragMode mode) {
    return mode == DragMode::kLeft || mode == DragMode::kTopLeft
        || mode == DragMode::kBottomLeft;
  }
  static bool has_right(DragMode mode) {
    return mode == DragMode::kRight || mode == DragMode::kTopRight
        || mode == DragMode::kBottomRight;
  }
  static bool has_top(DragMode mode) {
    return mode == DragMode::kTop || mode == DragMode::kTopLeft
        || mode == DragMode::kTopRight;
  }
  static bool has_bottom(DragMode mode) {
    return mode == DragMode::kBottom || mode == DragMode::kBottomLeft
        || mode == DragMode::kBottomRight;
  }

  DesktopNavigationPanel* panel_ = nullptr;
  QHash<QString, DisplayMemory> memories_;
  QString display_id_;
  QSize source_size_;
  bool dragging_ = false;
  DragMode drag_mode_ = DragMode::kNone;
  QPointF drag_start_;
  QRectF drag_region_;
  QRectF drag_content_rect_;
};

DesktopNavigationPanel::DesktopNavigationPanel(QWidget* parent)
    : QWidget(parent) {
  setObjectName("desktopNavigationPanel");
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(6);
  display_combo_ = new QComboBox(this);
  resolution_label_ = new QLabel("No display catalog", this);
  resolution_label_->setStyleSheet("color: #94a3b8;");
  status_label_ = new QLabel("Waiting for connection", this);
  status_label_->setStyleSheet("color: #94a3b8;");
  selection_widget_ = new NavigationSelectionWidget(this);
  layout->addWidget(display_combo_);
  layout->addWidget(resolution_label_);
  layout->addWidget(selection_widget_, 1);
  layout->addWidget(status_label_);
  connect(display_combo_, qOverload<int>(&QComboBox::currentIndexChanged),
          this, [this](int index) { select_display(index, true); });
}

void DesktopNavigationPanel::set_region_request_callback(
    RegionRequestCallback callback) {
  request_callback_ = std::move(callback);
}

void DesktopNavigationPanel::set_display_catalog(
    const std::vector<redclaw::protocol::DesktopDisplayV1>& displays,
    std::uint64_t catalog_revision) {
  if (catalog_revision == 0 || catalog_revision < catalog_revision_ || displays.empty()) {
    return;
  }
  const QString prior = selected_display_id();
  catalog_revision_ = catalog_revision;
  displays_ = displays;
  display_combo_->blockSignals(true);
  display_combo_->clear();
  int selected = 0;
  for (std::size_t index = 0; index < displays_.size(); ++index) {
    const auto& display = displays_[index];
    const QString id = QString::fromStdString(display.display_id);
    display_combo_->addItem(QString::fromStdString(display.display_name), id);
    if (id == prior || (prior.isEmpty() && display.primary)) {
      selected = static_cast<int>(index);
    }
  }
  display_combo_->setCurrentIndex(selected);
  display_combo_->blockSignals(false);
  select_display(selected, false);
  if (confirmed_display_id_.isEmpty()
      || display_combo_->findData(confirmed_display_id_) < 0) {
    confirmed_display_id_ = selected_display_id();
  }
  status_label_->setText(transport_available_ ? "Ready" : "Waiting for navigation channel");
}

void DesktopNavigationPanel::set_thumbnail(
    const std::string& display_id,
    const QImage& thumbnail,
    std::uint64_t thumbnail_revision) {
  selection_widget_->set_thumbnail(
      QString::fromStdString(display_id), thumbnail, thumbnail_revision);
}

void DesktopNavigationPanel::apply_region_applied(
    const redclaw::protocol::StreamControlMessageV1& message) {
  if (message.capture_region_revision < pending_region_revision_
      || QString::fromStdString(message.display_id) != selected_display_id()) {
    return;
  }
  const QRectF confirmed(
      static_cast<qreal>(message.region_left) / 65535.0,
      static_cast<qreal>(message.region_top) / 65535.0,
      static_cast<qreal>(message.region_right - message.region_left) / 65535.0,
      static_cast<qreal>(message.region_bottom - message.region_top) / 65535.0);
  selection_widget_->set_confirmed_region(confirmed);
  confirmed_display_id_ = QString::fromStdString(message.display_id);
  next_region_revision_ = (std::max)(
      next_region_revision_, message.capture_region_revision + 1);
  pending_region_revision_ = 0;
  status_label_->setText(QString("Applied revision %1").arg(message.capture_region_revision));
}

void DesktopNavigationPanel::apply_region_rejected(
    const redclaw::protocol::StreamControlMessageV1& message) {
  if (message.capture_region_revision != pending_region_revision_) return;
  selection_widget_->rollback();
  pending_region_revision_ = 0;
  const int confirmed_index = display_combo_->findData(confirmed_display_id_);
  if (confirmed_index >= 0 && confirmed_index != display_combo_->currentIndex()) {
    display_combo_->blockSignals(true);
    display_combo_->setCurrentIndex(confirmed_index);
    display_combo_->blockSignals(false);
    select_display(confirmed_index, false);
  }
  status_label_->setText(
      QString("Rejected: %1").arg(QString::fromStdString(message.payload)));
}

void DesktopNavigationPanel::set_transport_available(bool available) {
  transport_available_ = available;
  display_combo_->setEnabled(available && !workspace_blocked_ && !displays_.empty());
  status_label_->setText(available ? "Ready" : "Waiting for navigation channel");
}

void DesktopNavigationPanel::set_workspace_blocked(bool blocked) {
  workspace_blocked_ = blocked;
  display_combo_->setEnabled(transport_available_ && !blocked && !displays_.empty());
  selection_widget_->setEnabled(!blocked);
}

QString DesktopNavigationPanel::selected_display_id() const {
  return display_combo_->currentData().toString();
}

QString DesktopNavigationPanel::confirmed_display_id() const {
  return confirmed_display_id_;
}

QRectF DesktopNavigationPanel::selected_normalized_region() const {
  return selection_widget_->region();
}

std::uint64_t DesktopNavigationPanel::pending_region_revision() const {
  return pending_region_revision_;
}

bool DesktopNavigationPanel::pending_request_changes_display() const {
  return !confirmed_display_id_.isEmpty()
      && selected_display_id() != confirmed_display_id_;
}

void DesktopNavigationPanel::select_display(int index, bool submit) {
  if (index < 0 || index >= static_cast<int>(displays_.size())) return;
  const auto& display = displays_[static_cast<std::size_t>(index)];
  selection_widget_->set_display(
      QString::fromStdString(display.display_id),
      QSize(static_cast<int>(display.pixel_width), static_cast<int>(display.pixel_height)));
  update_display_text();
  if (submit) commit_current_selection();
}

void DesktopNavigationPanel::commit_current_selection() {
  if (!transport_available_ || workspace_blocked_ || !request_callback_ || current_display() == nullptr) {
    return;
  }
  const QRectF region = selection_widget_->region();
  const auto to_bound = [](qreal value) {
    return static_cast<std::uint16_t>(
        std::clamp(std::llround(value * 65535.0), 0LL, 65535LL));
  };
  const std::uint64_t revision = next_region_revision_++;
  pending_region_revision_ = revision;
  status_label_->setText(QString("Applying revision %1...").arg(revision));
  if (request_callback_(
      selected_display_id().toStdString(),
      to_bound(region.left()),
      to_bound(region.top()),
      to_bound(region.right()),
      to_bound(region.bottom()),
      revision)) {
    return;
  }
  selection_widget_->rollback();
  pending_region_revision_ = 0;
  const int confirmed_index = display_combo_->findData(confirmed_display_id_);
  if (confirmed_index >= 0 && confirmed_index != display_combo_->currentIndex()) {
    display_combo_->blockSignals(true);
    display_combo_->setCurrentIndex(confirmed_index);
    display_combo_->blockSignals(false);
    select_display(confirmed_index, false);
  }
  status_label_->setText("Region request could not be sent");
}

void DesktopNavigationPanel::update_display_text() {
  const auto* display = current_display();
  if (display == nullptr) {
    resolution_label_->setText("No display catalog");
    return;
  }
  resolution_label_->setText(QString("%1 x %2  |  rotation %3°")
      .arg(display->pixel_width)
      .arg(display->pixel_height)
      .arg(display->rotation));
}

const redclaw::protocol::DesktopDisplayV1* DesktopNavigationPanel::current_display() const {
  const int index = display_combo_->currentIndex();
  if (index < 0 || index >= static_cast<int>(displays_.size())) return nullptr;
  return &displays_[static_cast<std::size_t>(index)];
}

}  // namespace redclaw::ui
