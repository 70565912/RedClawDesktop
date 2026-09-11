#include "ui/playback_window_lifecycle.h"

#include <QEvent>
#include <QTimer>
#include <QWidget>

namespace redclaw::ui {

PlaybackWindowLifecycle::PlaybackWindowLifecycle(
    QWidget* playback_window,
    QWidget* return_window)
    : QObject(playback_window),
      playback_window_(playback_window),
      return_window_(return_window) {
    if (playback_window_ == nullptr) {
        return;
    }

    playback_window_->setAttribute(Qt::WA_DeleteOnClose, false);
    playback_window_->installEventFilter(this);
}

bool PlaybackWindowLifecycle::eventFilter(QObject* watched, QEvent* event) {
    if (watched == playback_window_ && event != nullptr && event->type() == QEvent::Close) {
        const QPointer<QWidget> return_window = return_window_;
        if (return_window != nullptr) {
            if (return_window->isMinimized()) {
                return_window->showNormal();
            } else {
                return_window->show();
            }
            return_window->raise();
            return_window->activateWindow();
            QTimer::singleShot(0, return_window, [return_window]() {
                if (return_window != nullptr) {
                    return_window->raise();
                    return_window->activateWindow();
                }
            });
        }
    }

    return QObject::eventFilter(watched, event);
}

}  // namespace redclaw::ui
