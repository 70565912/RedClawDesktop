#pragma once
#include "ui/connection_flow_model.h"
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

class QMainWindow;
class QSettings;
class QPushButton;
class QWidget;
namespace redclaw::ui {
class ConnectionOverlay;

enum class SessionDisconnectState { kIdle, kStopping, kFailed };

// One top-level window. Only presentation/lifecycle requests live here;
// the runtime controller remains the authority for actual session shutdown.
class UnifiedSessionWorkspace final : public QObject {
public:
    UnifiedSessionWorkspace(QMainWindow* window, QWidget* home, QWidget* log, QSettings* settings);
    ~UnifiedSessionWorkspace() override;
    [[nodiscard]] QWidget* desktop_page() const;
    [[nodiscard]] ConnectionOverlay* connection_overlay() const;
    [[nodiscard]] bool desktop_visible() const;
    [[nodiscard]] SessionDisconnectState disconnect_state() const;
    void update_connection(const ConnectionFlowSnapshot& snapshot);
    void update_runtime(const QString& transport, const QString& frame);
    void present_desktop();
    void request_disconnect();
    void runtime_stopped();
    void stop_failed(const QString& reason);
    void bind_disconnect_button(QPushButton* button);
    void set_confirm_disconnect(std::function<bool()> callback);
    void set_request_stop(std::function<void()> callback);
    void set_runtime_running(std::function<bool()> callback);
    void set_before_home(std::function<void()> callback);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace redclaw::ui
