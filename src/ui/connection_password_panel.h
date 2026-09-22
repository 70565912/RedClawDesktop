#pragma once
#include <QObject>
#include <QString>
class QWidget;
#include <functional>
#include "redclaw/security/connection_auth.h"

class QSettings;
class QLineEdit;

namespace redclaw::ui {
class ConnectionPasswordPanel final : public QObject {
public:
    explicit ConnectionPasswordPanel(QWidget* parent = nullptr);
    ~ConnectionPasswordPanel() override;
    void set_settings(QSettings* settings);
    void set_peer_code(const QString& code);
    void set_busy(bool busy);
    bool host_ready() const;
    bool client_ready() const;
    bool prepare(bool host, security::ConnectionCredential* credential, QString* error);
    void accepted();
    void rejected();
    void set_credential_override(security::ConnectionCredential credential);
    void set_tab_order(QWidget* local_before, QWidget* local_after, QWidget* peer_before, QWidget* peer_after);
    QWidget* local_editor() const { return local_row_; }
    QWidget* peer_editor() const { return peer_row_; }
    std::function<void()> readiness_changed;
    std::function<void(const QString&)> status_changed;
private:
    void save_host();
    QString peer_key() const;
    void changed();
    QSettings* settings_ = nullptr;
    QLineEdit* local_ = nullptr;
    QWidget* local_row_ = nullptr;
    QWidget* peer_row_ = nullptr;
    QLineEdit* peer_ = nullptr;
    void report_status(const QString& text);
    void update_enabled();
    void hide_passwords();
    void update_placeholder();
    std::string verifier_, pending_password_;
    QString peer_code_, pending_code_;
    bool override_ = false;
    bool saving_ = false, busy_ = false;
    security::ConnectionCredential override_credential_;
};
} // namespace redclaw::ui
