#pragma once

#include <QObject>
#include <QPointer>
#include <QRect>
#include <functional>

class QWidget;

namespace redclaw::ui {

class PlaybackWindowFullscreenController final : public QObject {
public:
    PlaybackWindowFullscreenController(QWidget* playback_window, QWidget* shortcut_bar, QWidget* surface = nullptr);
    ~PlaybackWindowFullscreenController() override;

    void set_fullscreen(bool enabled);
    void toggle_fullscreen();
    [[nodiscard]] bool fullscreen() const;
    void set_changed_callback(std::function<void(bool)> callback);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    [[nodiscard]] QRect restored_geometry() const;
    QPointer<QWidget> window_;
    QPointer<QWidget> shortcut_bar_;
    QPointer<QWidget> surface_;
    QRect normal_geometry_;
    bool was_maximized_ = false;
    int suppressed_enter_ = 0;
    std::function<void(bool)> changed_;
};

} // namespace redclaw::ui
