#pragma once
#include <QWidget>
#include <functional>
#include "redclaw/security/connection_auth.h"

class QSettings;
class QLineEdit;
class QLabel;
namespace redclaw::ui {
class ConnectionPasswordPanel final : public QWidget {
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
    std::function<void()> readiness_changed;
private:
    void save_host();
    QString peer_key() const;
    void changed();
    QSettings* settings_ = nullptr;
    QLineEdit* local_ = nullptr;
    QLineEdit* confirmation_ = nullptr;
    QLineEdit* peer_ = nullptr;
    QLabel* status_ = nullptr;
    std::string verifier_, pending_password_;
    QString peer_code_, pending_code_;
    bool override_ = false;
    bool saving_ = false, busy_ = false;
    security::ConnectionCredential override_credential_;
};
} // namespace redclaw::ui
