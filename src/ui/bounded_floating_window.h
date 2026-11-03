#pragma once
#include "ui/desktop_workspace_theme.h"
#include <QWidget>
#include <QCloseEvent>
#include <QHideEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <algorithm>
#include <functional>

namespace redclaw::ui {
inline QRect bounded_floating_rect(QRect rect, const QRect& bounds) {
    rect.setSize(rect.size().expandedTo(QSize(1, 1)).boundedTo(bounds.size().expandedTo(QSize(1, 1))));
    rect.moveLeft(std::clamp(rect.x(), bounds.left(), bounds.right() - rect.width() + 1));
    rect.moveTop(std::clamp(rect.y(), bounds.top(), bounds.bottom() - rect.height() + 1));
    return rect;
}

// Owned native tool windows avoid the D3D11/WebView native-child stacking gap.
// Geometry remains strictly inside the owner's client rectangle, not the screen.
class BoundedFloatingWindow final : public QWidget {
public:
    BoundedFloatingWindow(QWidget* owner, bool translucent)
        : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint),
          translucent_(translucent) {
        setAttribute(Qt::WA_DeleteOnClose, false);
        setAttribute(Qt::WA_ShowWithoutActivating);
        if (translucent) setAttribute(Qt::WA_TranslucentBackground);
        setMouseTracking(true);
    }
    std::function<void()> dismissed, interacted, geometry_committed;
    void set_bounds(QRect bounds) {
        if (bounds != bounds_) stop_drag();
        bounds_ = bounds;
    }
    void set_resizable(bool enabled) { resizable_ = enabled; }
    void set_drag_handle(QWidget* handle) { handle->installEventFilter(this); handle->setCursor(Qt::SizeAllCursor); }
    [[nodiscard]] bool dragging() const { return moving_ || edges_ != Qt::Edges{}; }
protected:
    void hideEvent(QHideEvent* event) override { stop_drag(); QWidget::hideEvent(event); }
    void closeEvent(QCloseEvent* event) override {
        event->ignore();
        if (interacted) interacted();
        if (dismissed) dismissed();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QColor(desktop_theme::kBorder));
        auto color = QColor(desktop_theme::kSurface);
        color.setAlpha(translucent_ ? 242 : 255);
        painter.setBrush(color);
        painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 12, 12);
    }
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove
            || event->type() == QEvent::MouseButtonRelease) {
            return mouse(static_cast<QMouseEvent*>(event), qobject_cast<QWidget*>(watched), true);
        }
        return QWidget::eventFilter(watched, event);
    }
    void mousePressEvent(QMouseEvent* event) override { (void)mouse(event, this, false); }
    void mouseMoveEvent(QMouseEvent* event) override { (void)mouse(event, this, false); }
    void mouseReleaseEvent(QMouseEvent* event) override { (void)mouse(event, this, false); }
private:
    void stop_drag() {
        moving_ = false; edges_ = {};
        if (grabber_) grabber_->releaseMouse();
        grabber_.clear();
    }
    Qt::Edges hit_edges(QPoint point) const {
        Qt::Edges edges;
        if (translucent_ || !resizable_) return edges;
        if (point.x() < 6) edges |= Qt::LeftEdge;
        if (point.x() >= width() - 6) edges |= Qt::RightEdge;
        if (point.y() < 6) edges |= Qt::TopEdge;
        if (point.y() >= height() - 6) edges |= Qt::BottomEdge;
        return edges;
    }
    bool mouse(QMouseEvent* event, QWidget* target, bool handle) {
        const auto point = event->globalPosition().toPoint();
        if (event->type() == QEvent::MouseButtonPress && event->button() == Qt::LeftButton) {
            if (interacted) interacted();
            edges_ = handle ? Qt::Edges{} : hit_edges(mapFromGlobal(point));
            moving_ = handle;
            if (!dragging()) return false;
            origin_ = point; original_ = geometry(); grabber_ = target;
            target->grabMouse();
            return true;
        }
        if (event->type() == QEvent::MouseMove && dragging()) {
            const auto delta = point - origin_;
            QRect next = original_;
            if (moving_) next.translate(delta);
            else {
                const QSize minimum = QSize(240, 140).boundedTo(bounds_.size());
                if (edges_.testFlag(Qt::LeftEdge)) next.setLeft(std::clamp(original_.left() + delta.x(), bounds_.left(), original_.right() - minimum.width() + 1));
                if (edges_.testFlag(Qt::RightEdge)) next.setRight(std::clamp(original_.right() + delta.x(), original_.left() + minimum.width() - 1, bounds_.right()));
                if (edges_.testFlag(Qt::TopEdge)) next.setTop(std::clamp(original_.top() + delta.y(), bounds_.top(), original_.bottom() - minimum.height() + 1));
                if (edges_.testFlag(Qt::BottomEdge)) next.setBottom(std::clamp(original_.bottom() + delta.y(), original_.top() + minimum.height() - 1, bounds_.bottom()));
            }
            setGeometry(bounded_floating_rect(next, bounds_));
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease && event->button() == Qt::LeftButton && dragging()) {
            stop_drag();
            if (geometry_committed) geometry_committed();
            return true;
        }
        if (!handle && !dragging()) {
            const auto edges = hit_edges(mapFromGlobal(point));
            const bool horizontal = edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge);
            const bool vertical = edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge);
            setCursor(horizontal && vertical
                ? ((edges.testFlag(Qt::LeftEdge) == edges.testFlag(Qt::TopEdge)) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor)
                : horizontal ? Qt::SizeHorCursor : vertical ? Qt::SizeVerCursor : Qt::ArrowCursor);
        }
        return false;
    }
    bool translucent_, moving_ = false, resizable_ = true;
    Qt::Edges edges_;
    QRect bounds_, original_;
    QPoint origin_;
    QPointer<QWidget> grabber_;
};

} // namespace redclaw::ui
