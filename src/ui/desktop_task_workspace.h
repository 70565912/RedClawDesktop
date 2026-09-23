#pragma once

#include <QObject>
#include <QSize>
#include <QString>
#include <functional>
#include <memory>

class QLabel;
class QPushButton;
class QSettings;
class QToolButton;
class QWidget;

namespace redclaw::ui {

enum class DesktopTask { kAgent, kTerminal, kNavigation, kFiles };

// Presentation only: never owns remote tasks, sessions or their stop operations.
class DesktopTaskWorkspace final : public QObject {
public:
    DesktopTaskWorkspace(QWidget* playback_window, QSettings* settings);
    ~DesktopTaskWorkspace() override;
    void add_task(DesktopTask id, const QString& title, QWidget* content, QSize initial_size);
    void set_task_visible(DesktopTask id, bool visible);
    [[nodiscard]] bool task_visible(DesktopTask id) const;
    [[nodiscard]] QWidget* task_window(DesktopTask id) const;
    [[nodiscard]] QToolButton* task_button(DesktopTask id) const;
    [[nodiscard]] QWidget* button_bar() const;
    [[nodiscard]] QPushButton* control_button() const;
    [[nodiscard]] QPushButton* audio_button() const;
    [[nodiscard]] QPushButton* retry_button() const;
    [[nodiscard]] QLabel* control_status() const;
    [[nodiscard]] QLabel* connection_status() const;
    void set_enabled(bool enabled);
    void set_local_interaction_callback(std::function<void()> callback);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace redclaw::ui
