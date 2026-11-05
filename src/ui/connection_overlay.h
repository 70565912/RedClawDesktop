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
enum class ConnectionPairingState { kReady, kConnecting, kConnected, kAttention, kClosed };
enum class ConnectionProcessState { kIdle, kActive, kStopping, kFailed };

// Presentation only. The session owns transport. Diagnostics stay in log files.
class ConnectionOverlay final : public QObject {
public:
    ConnectionOverlay(QWidget* surface, QSettings* settings);
    ~ConnectionOverlay() override;
    void set_active(bool active);
    void set_state(ConnectionOverlayState state);
    [[nodiscard]] ConnectionOverlayState state() const;
    void update_connection(const ConnectionFlowSnapshot& snapshot);
    void update_runtime(const QString& transport, const QString& frame);
    void set_pairing_state(ConnectionPairingState state);
    void set_handshake_ready(bool ready);
    void set_link_live(bool live);
    void set_process_state(ConnectionProcessState state);
    void show_stop_failure(const QString& message);
    void bind_button(QToolButton* button);
    void set_local_interaction_callback(std::function<void()> callback);
    void set_disconnect_callback(std::function<void()> callback);
    void set_reconnect_callback(std::function<void()> callback);
    [[nodiscard]] QWidget* panel() const;
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace redclaw::ui
