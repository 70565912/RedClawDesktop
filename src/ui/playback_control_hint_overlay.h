#pragma once

#include <QWidget>

class QTimer;

namespace redclaw::ui {

class PlaybackControlHintOverlay final : public QWidget {
public:
    explicit PlaybackControlHintOverlay(QWidget* canvas, bool hints_enabled = true);

    void show_hint(const QString& message, int duration_ms = 2000);
    void show_clipboard_failure(const QString& message);
    void hide_hint();
    void hide_control_hint();
    void hide_persistent_hint();
    [[nodiscard]] QString message() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    enum class HintKind { kControl, kClipboardFailure, kPersistent };

    void show_hint(const QString& message, int duration_ms, HintKind kind);
    void reposition();

    QWidget* canvas_ = nullptr;
    QWidget* playback_window_ = nullptr;
    const bool hints_enabled_;
    QTimer* hide_timer_ = nullptr;
    QString message_;
    HintKind kind_ = HintKind::kControl;
};

}  // namespace redclaw::ui
