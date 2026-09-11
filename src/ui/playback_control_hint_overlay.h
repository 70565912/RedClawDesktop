#pragma once

#include <QWidget>

class QTimer;

namespace redclaw::ui {

class PlaybackControlHintOverlay final : public QWidget {
public:
    explicit PlaybackControlHintOverlay(QWidget* canvas);

    void show_hint(const QString& message, int duration_ms = 2000);
    void hide_hint();
    void hide_persistent_hint();
    [[nodiscard]] QString message() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void reposition();

    QWidget* canvas_ = nullptr;
    QWidget* playback_window_ = nullptr;
    QTimer* hide_timer_ = nullptr;
    QString message_;
};

}  // namespace redclaw::ui
