#pragma once

#include <QObject>
#include <QPointer>

class QEvent;
class QWidget;

namespace redclaw::ui {

class PlaybackWindowLifecycle final : public QObject {
public:
    PlaybackWindowLifecycle(QWidget* playback_window, QWidget* return_window);

    PlaybackWindowLifecycle(const PlaybackWindowLifecycle&) = delete;
    PlaybackWindowLifecycle& operator=(const PlaybackWindowLifecycle&) = delete;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QPointer<QWidget> playback_window_;
    QPointer<QWidget> return_window_;
};

}  // namespace redclaw::ui
