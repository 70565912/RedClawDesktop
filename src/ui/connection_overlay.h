#pragma once

#include "ui/connection_flow_model.h"
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

class QSettings;
class QToolButton;
class QWidget;

namespace redclaw::ui {
enum class ConnectionOverlayState { kExpanded, kCollapsed, kHidden };

// Presentation only. The existing session owns transport and log storage.
class ConnectionOverlay final : public QObject {
public:
    ConnectionOverlay(QWidget* surface, QWidget* log, QSettings* settings);
    ~ConnectionOverlay() override;
    void set_active(bool active);
    void set_state(ConnectionOverlayState state);
    [[nodiscard]] ConnectionOverlayState state() const;
    void update_connection(const ConnectionFlowSnapshot& snapshot);
    void update_runtime(const QString& transport, const QString& frame);
    void show_stop_failure(const QString& message);
    void bind_button(QToolButton* button);
    void set_local_interaction_callback(std::function<void()> callback);
    void set_disconnect_callback(std::function<void()> callback);
    void set_reconnect_callback(std::function<void()> callback);
    [[nodiscard]] QWidget* panel() const;
    [[nodiscard]] QWidget* status_entry() const;
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace redclaw::ui
