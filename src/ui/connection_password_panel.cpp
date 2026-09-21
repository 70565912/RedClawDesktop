#include "ui/connection_password_panel.h"
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

namespace redclaw::ui {
ConnectionPasswordPanel::ConnectionPasswordPanel(QWidget* parent) : QWidget(parent) {
    setObjectName("connectionPasswordPanel");
    auto* layout = new QFormLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    local_ = new QLineEdit(this); local_->setObjectName("localConnectionPassword");
    confirmation_ = new QLineEdit(this); confirmation_->setObjectName("confirmConnectionPassword");
    peer_ = new QLineEdit(this); peer_->setObjectName("peerConnectionPassword");
    for (auto* input : {local_, confirmation_, peer_}) {
        input->setEchoMode(QLineEdit::Password);
        input->setMaxLength(static_cast<int>(security::kMaxConnectionPasswordBytes));
    }
    local_->setPlaceholderText("Set or change this computer's password");
    confirmation_->setPlaceholderText("Enter the same password again");
    peer_->setPlaceholderText("Enter the Host's connection password");
    layout->addRow("Local connection password", local_);
    auto* save_row = new QHBoxLayout(); save_row->addWidget(confirmation_);
    auto* save = new QPushButton("Save", this); save->setObjectName("saveConnectionPassword");
    save_row->addWidget(save); layout->addRow("Confirm password", save_row);
    auto* peer_row = new QHBoxLayout(); peer_row->addWidget(peer_);
    auto* forget = new QPushButton("Forget", this); forget->setObjectName("forgetConnectionPassword");
    peer_row->addWidget(forget); layout->addRow("Peer connection password", peer_row);
    status_ = new QLabel("Set a local password before waiting for connections.", this);
    status_->setObjectName("connectionPasswordStatus"); status_->setWordWrap(true);
    layout->addRow(status_);
    QObject::connect(save, &QPushButton::clicked, this, [this] { save_host(); });
    QObject::connect(peer_, &QLineEdit::textChanged, this, [this] { changed(); });
    QObject::connect(forget, &QPushButton::clicked, this, [this] {
        if (settings_) { settings_->remove(peer_key()); settings_->sync(); }
        peer_->clear(); security::erase_secret(pending_password_); pending_code_.clear();
        status_->setText("Saved peer password removed."); changed();
    });
}
ConnectionPasswordPanel::~ConnectionPasswordPanel() {
    security::erase_secret(verifier_); security::erase_secret(pending_password_);
    security::erase_secret(override_credential_.secret);
}
void ConnectionPasswordPanel::changed() { if (readiness_changed) readiness_changed(); }
QString ConnectionPasswordPanel::peer_key() const { return "connectionAuth/v1/peers/" + peer_code_; }
void ConnectionPasswordPanel::set_settings(QSettings* settings) {
    settings_ = settings; security::erase_secret(verifier_);
    const auto protected_value = settings_->value("connectionAuth/v1/host").toString().toStdString();
    if (!protected_value.empty()) {
        if (!security::unprotect_connection_credential(protected_value, &verifier_)
            || !security::valid_connection_verifier(verifier_)) {
            security::erase_secret(verifier_);
            status_->setText("Saved local password could not be decrypted. Set a new password.");
        } else status_->setText("Local connection password is set.");
    }
    changed();
}
void ConnectionPasswordPanel::save_host() {
    if (!settings_ || saving_ || busy_) return;
    if (local_->text().isEmpty() || local_->text() != confirmation_->text()) {
        status_->setText("Enter a nonempty password and matching confirmation."); return;
    }
    auto password = local_->text().toUtf8().toStdString();
    struct SavedPassword { std::string verifier, encrypted; bool ok = false; };
    saving_ = true; setEnabled(false); changed();
    status_->setText("Saving connection password...");
    auto* watcher = new QFutureWatcher<SavedPassword>(this);
    QObject::connect(watcher, &QFutureWatcher<SavedPassword>::finished, this, [this, watcher] {
        auto result = watcher->result(); watcher->deleteLater();
        saving_ = false; setEnabled(!busy_);
        if (result.ok) {
            settings_->setValue("connectionAuth/v1/host", QString::fromStdString(result.encrypted));
            settings_->sync();
            result.ok = settings_->status() == QSettings::NoError;
        }
        if (result.ok) {
            security::erase_secret(verifier_); verifier_ = std::move(result.verifier);
            local_->clear(); confirmation_->clear();
            status_->setText("Local connection password saved.");
        } else status_->setText("Could not protect or save the password for this Windows user.");
        security::erase_secret(result.verifier); changed();
    });
    watcher->setFuture(QtConcurrent::run([password = std::move(password)]() mutable {
        SavedPassword result;
        result.ok = security::make_connection_verifier(password, &result.verifier)
            && security::protect_connection_credential(result.verifier, &result.encrypted);
        security::erase_secret(password); return result;
    }));
}
void ConnectionPasswordPanel::set_peer_code(const QString& code) {
    if (peer_code_ == code) return;
    peer_code_ = code; peer_->clear();
    if (!settings_ || code.size() != 8) return;
    const auto encrypted = settings_->value(peer_key()).toString().toStdString();
    if (encrypted.empty()) return;
    std::string password;
    if (security::unprotect_connection_credential(encrypted, &password)) {
        peer_->setText(QString::fromUtf8(password.data(), static_cast<int>(password.size())));
        security::erase_secret(password);
    } else status_->setText("Saved peer password could not be decrypted. Enter it again.");
    changed();
}
void ConnectionPasswordPanel::set_busy(bool busy) { busy_ = busy; setEnabled(!busy && !saving_); }
bool ConnectionPasswordPanel::host_ready() const { return !saving_ && ((override_ && override_credential_.host) || !verifier_.empty()); }
bool ConnectionPasswordPanel::client_ready() const { return !saving_ && ((override_ && !override_credential_.host) || !peer_->text().isEmpty()); }
bool ConnectionPasswordPanel::prepare(bool host, security::ConnectionCredential* credential, QString* error) {
    security::erase_secret(pending_password_); pending_code_.clear();
    if (override_) {
        if (override_credential_.host != host) { *error = "Credential belongs to a different role."; return false; }
        *credential = override_credential_; return true;
    }
    if (host ? !host_ready() : !client_ready()) {
        *error = host ? "Set a local connection password before waiting." : "Enter the Host's connection password.";
        return false;
    }
    *credential = {host, host ? verifier_ : peer_->text().toUtf8().toStdString()};
    if (credential->secret.size() > (host ? 4096U : security::kMaxConnectionPasswordBytes)) {
        security::erase_secret(credential->secret); *error = "Connection password is too long."; return false;
    }
    if (!host) { pending_password_ = credential->secret; pending_code_ = peer_code_; }
    return true;
}
void ConnectionPasswordPanel::accepted() {
    if (!settings_ || override_ || pending_password_.empty() || pending_code_.size() != 8) return;
    std::string encrypted;
    if (security::protect_connection_credential(pending_password_, &encrypted)) {
        settings_->setValue("connectionAuth/v1/peers/" + pending_code_, QString::fromStdString(encrypted)); settings_->sync();
    }
    security::erase_secret(pending_password_); pending_code_.clear();
}
void ConnectionPasswordPanel::rejected() {
    security::erase_secret(pending_password_); pending_code_.clear();
    if (settings_ && !override_) { settings_->remove(peer_key()); settings_->sync(); }
    peer_->clear(); status_->setText("Host rejected the connection password. Enter it again."); changed();
}
void ConnectionPasswordPanel::set_credential_override(security::ConnectionCredential credential) {
    security::erase_secret(override_credential_.secret); override_credential_ = std::move(credential); override_ = true; changed();
}
} // namespace redclaw::ui
