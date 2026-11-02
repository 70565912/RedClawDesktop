#include "ui/playback_window_fullscreen_controller.h"

#include <QApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <utility>

namespace redclaw::ui {

PlaybackWindowFullscreenController::PlaybackWindowFullscreenController(QWidget* window, QWidget* shortcut_bar, QWidget* surface)
    : QObject(window), window_(window), shortcut_bar_(shortcut_bar), surface_(surface) {
    qApp->installEventFilter(this);
}

PlaybackWindowFullscreenController::~PlaybackWindowFullscreenController() {
    qApp->removeEventFilter(this);
}

bool PlaybackWindowFullscreenController::fullscreen() const {
    return window_ && window_->isFullScreen();
}

void PlaybackWindowFullscreenController::set_changed_callback(std::function<void(bool)> callback) {
    changed_ = std::move(callback);
    if (changed_) changed_(fullscreen());
}

QRect PlaybackWindowFullscreenController::restored_geometry() const {
    QRect rect = normal_geometry_;
    QScreen* screen = QGuiApplication::screenAt(rect.center());
    if (!screen && window_) screen = window_->screen();
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (!screen) return rect;
    const QRect available = screen->availableGeometry();
    rect.setSize(rect.size().boundedTo(available.size()));
    rect.moveLeft(std::clamp(rect.left(), available.left(), available.right() - rect.width() + 1));
    rect.moveTop(std::clamp(rect.top(), available.top(), available.bottom() - rect.height() + 1));
    return rect;
}

void PlaybackWindowFullscreenController::set_fullscreen(bool enabled) {
    if (!window_ || enabled == fullscreen() || (enabled && surface_ && !surface_->isVisible())) return;
    if (enabled) {
        if (!window_->isVisible() || window_->isMinimized()) return;
        was_maximized_ = window_->isMaximized();
        normal_geometry_ = window_->normalGeometry();
        if (!normal_geometry_.isValid()) normal_geometry_ = window_->geometry();
        window_->showFullScreen();
    } else {
        window_->showNormal();
        if (normal_geometry_.isValid()) window_->setGeometry(restored_geometry());
        if (was_maximized_) window_->showMaximized();
    }
}

void PlaybackWindowFullscreenController::toggle_fullscreen() {
    set_fullscreen(!fullscreen());
}

bool PlaybackWindowFullscreenController::eventFilter(QObject* watched, QEvent* event) {
    if (!window_ || !event) return false;
    if (watched == window_) {
        if (event->type() == QEvent::Close && !surface_ && fullscreen()) set_fullscreen(false);
        if (event->type() == QEvent::WindowStateChange && changed_) changed_(fullscreen());
    }
    if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress
        && event->type() != QEvent::KeyRelease) return false;
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget || (surface_ && !surface_->isVisible()) || !window_->isVisible() || window_->isMinimized()) return false;
    auto* top = widget->window();
    // The terminal/Agent are owned native tools, but keep their shortcuts local
    // to those editors rather than treating every owner descendant as a canvas.
    if (top != window_ && top != shortcut_bar_) return false;
    auto* key = static_cast<QKeyEvent*>(event);
    if (suppressed_enter_ == key->key()) {
        if (event->type() == QEvent::KeyRelease && !key->isAutoRepeat()) suppressed_enter_ = 0;
        key->accept(); return true;
    }
    const bool chord = (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
        && key->modifiers() == (Qt::ControlModifier | Qt::AltModifier);
    if (!chord || event->type() == QEvent::KeyRelease) return false;
    key->accept();
    if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()) {
        suppressed_enter_ = key->key();
        QTimer::singleShot(0, this, [this] { toggle_fullscreen(); });
    }
    return true;
}

} // namespace redclaw::ui
