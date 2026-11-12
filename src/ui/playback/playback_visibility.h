#pragma once
#include <QEvent>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>

namespace redclaw::ui {
// Raster/OpenGL visibility scope: hide/show and minimize/restore. DXGI
// occlusion and Windows display power are handled by the D3D11 canvas.
class PlaybackVisibility final : public QObject {
public:
    explicit PlaybackVisibility(QWidget* widget) : QObject(widget), widget_(widget) {
        timer_.setInterval(250);
        connect(&timer_, &QTimer::timeout, this, [this] { synchronize(); });
        widget_->installEventFilter(this);
        timer_.start();
    }
    void set_callback(std::function<void(bool, const QString&)> callback) {
        callback_ = std::move(callback);
        initialized_ = false;
        synchronize();
    }
private:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Show || event->type() == QEvent::Hide
            || event->type() == QEvent::WindowStateChange) synchronize();
        return QObject::eventFilter(object, event);
    }
    void synchronize() {
        if (top_ != widget_->window()) {
            if (top_) top_->removeEventFilter(this);
            top_ = widget_->window();
            if (top_) top_->installEventFilter(this);
        }
        const bool paused = !widget_->isVisible() || (top_ && top_->isMinimized());
        if (paused) {
            if (!timer_.isActive()) timer_.start();
        } else if (timer_.isActive()) timer_.stop();
        if (initialized_ && paused == paused_) return;
        initialized_ = true;
        paused_ = paused;
        if (callback_) callback_(paused, paused ? "hidden" : "shown");
    }
    QWidget* widget_;
    QPointer<QWidget> top_;
    QTimer timer_;
    bool initialized_ = false, paused_ = false;
    std::function<void(bool, const QString&)> callback_;
};
}  // namespace redclaw::ui
