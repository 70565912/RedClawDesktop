#include "ui/connection_password_panel.h"
#include <QToolButton>
#include <QProxyStyle>
#include <QResizeEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

namespace redclaw::ui {
namespace {
class PasswordStyle final : public QProxyStyle {
public:
    int styleHint(StyleHint hint, const QStyleOption* option = nullptr,
                  const QWidget* widget = nullptr, QStyleHintReturn* data = nullptr) const override {
        return hint == SH_LineEdit_PasswordCharacter ? '*' : QProxyStyle::styleHint(hint, option, widget, data);
    }
};
class PasswordEdit final : public QLineEdit {
public:
    PasswordEdit(const QString& name, QWidget* parent) : QLineEdit(parent) {
        setObjectName(name);
        setProperty("connectionField", true);
        setFixedHeight(52);
        auto* password_style = new PasswordStyle();
        password_style->setParent(this);
        setStyle(password_style);
        setEchoMode(QLineEdit::Password);
        setMaxLength(static_cast<int>(security::kMaxConnectionPasswordBytes));
        toggle_ = new QToolButton(this);
        toggle_->setObjectName(name + "Visibility");
        toggle_->setText("Show");
        toggle_->setAccessibleName("Show password");
        toggle_->setFocusPolicy(Qt::StrongFocus);
        toggle_->setStyleSheet("QToolButton { border: none; background: transparent; color: #93c5fd; padding: 0px; font: 9pt 'Segoe UI'; } QToolButton:focus { border: 1px solid #7dd3fc; }");
        setTextMargins(0, 0, 48, 0);
        QObject::connect(toggle_, &QToolButton::clicked, this, [this] {
            const int cursor = cursorPosition();
            const int start = selectionStart();
            const int length = selectedText().size();
            setEchoMode(echoMode() == QLineEdit::Password ? QLineEdit::Normal : QLineEdit::Password);
            if (length > 0) setSelection(cursor == start ? start + length : start, cursor == start ? -length : length);
            else setCursorPosition(cursor);
            update_toggle();
        });
    }
    void conceal() { setEchoMode(QLineEdit::Password); update_toggle(); }
protected:
    void resizeEvent(QResizeEvent* event) override {
        QLineEdit::resizeEvent(event);
        toggle_->setGeometry(width() - 58, 8, 48, height() - 16);
    }
private:
    void update_toggle() {
        const bool hidden = echoMode() == QLineEdit::Password;
        toggle_->setText(hidden ? "Show" : "Hide");
        toggle_->setAccessibleName(hidden ? "Show password" : "Hide password");
    }
    QToolButton* toggle_ = nullptr;
};
}
ConnectionPasswordPanel::ConnectionPasswordPanel(QWidget* parent) : QObject(parent) {
    setObjectName("connectionPasswordPanel");
    local_row_ = new QWidget(parent);
    peer_row_ = new QWidget(parent);
    local_ = new PasswordEdit("localConnectionPassword", local_row_);
    peer_ = new PasswordEdit("peerConnectionPassword", peer_row_);
    local_->setPlaceholderText("Set this computer's password");
    peer_->setPlaceholderText("Enter the Host's password");
    auto* save = new QPushButton("Save", local_row_);
    save->setObjectName("saveConnectionPassword");
    auto* forget = new QPushButton("Forget", peer_row_);
    forget->setObjectName("forgetConnectionPassword");
    for (auto pair : {std::make_pair(local_row_, local_), std::make_pair(peer_row_, peer_)}) {
        auto* layout = new QHBoxLayout(pair.first);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(5);
        layout->addWidget(pair.second, 1);
        pair.first->setFocusProxy(pair.second);
    }
    local_row_->layout()->addWidget(save);
    peer_row_->layout()->addWidget(forget);
    QObject::connect(save, &QPushButton::clicked, this, [this] { save_host(); });
    QObject::connect(local_, &QLineEdit::textChanged, this, [this] { changed(); });
    QObject::connect(peer_, &QLineEdit::textChanged, this, [this] { changed(); });
    QObject::connect(forget, &QPushButton::clicked, this, [this] {
        hide_passwords();
        if (settings_) { settings_->remove(peer_key()); settings_->sync(); }
        peer_->clear(); security::erase_secret(pending_password_); pending_code_.clear();
        report_status("Saved peer password removed."); changed();
    });
}
void ConnectionPasswordPanel::set_tab_order(QWidget* local_before, QWidget* local_after,
                                             QWidget* peer_before, QWidget* peer_after) {
    QWidget::setTabOrder(local_before, local_);
    QWidget::setTabOrder(local_, local_->findChild<QToolButton*>());
    QWidget::setTabOrder(local_->findChild<QToolButton*>(), local_row_->findChild<QPushButton*>());
    QWidget::setTabOrder(local_row_->findChild<QPushButton*>(), local_after);
    QWidget::setTabOrder(local_after, peer_before);
    QWidget::setTabOrder(peer_before, peer_);
    QWidget::setTabOrder(peer_, peer_->findChild<QToolButton*>());
    QWidget::setTabOrder(peer_->findChild<QToolButton*>(), peer_row_->findChild<QPushButton*>());
    QWidget::setTabOrder(peer_row_->findChild<QPushButton*>(), peer_after);
}
void ConnectionPasswordPanel::report_status(const QString& text) { if (status_changed) status_changed(text); }
void ConnectionPasswordPanel::update_enabled() {
    local_row_->setEnabled(!busy_ && !saving_);
    peer_row_->setEnabled(!busy_ && !saving_);
}
void ConnectionPasswordPanel::hide_passwords() {
    static_cast<PasswordEdit*>(local_)->conceal();
    static_cast<PasswordEdit*>(peer_)->conceal();
}
void ConnectionPasswordPanel::update_placeholder() {
    local_->setPlaceholderText(!verifier_.empty() || (override_ && override_credential_.host)
        ? "Set; enter a new password to change" : "Set this computer's password");
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
            report_status("Saved local password could not be decrypted. Set a new password.");
        } else report_status("Local connection password is set.");
    }
    update_placeholder(); changed();
}
void ConnectionPasswordPanel::save_host() {
    if (!settings_ || saving_ || busy_) return;
    if (local_->text().isEmpty()) {
        report_status("Enter a nonempty password."); return;
    }
    auto password = local_->text().toUtf8().toStdString();
    struct SavedPassword { std::string verifier, encrypted; bool ok = false; };
    saving_ = true; update_enabled(); changed();
    report_status("Saving connection password...");
    auto* watcher = new QFutureWatcher<SavedPassword>(this);
    QObject::connect(watcher, &QFutureWatcher<SavedPassword>::finished, this, [this, watcher] {
        auto result = watcher->result(); watcher->deleteLater();
        saving_ = false; update_enabled();
        if (result.ok) {
            settings_->setValue("connectionAuth/v1/host", QString::fromStdString(result.encrypted));
            settings_->sync();
            result.ok = settings_->status() == QSettings::NoError;
        }
        if (result.ok) {
            security::erase_secret(verifier_); verifier_ = std::move(result.verifier);
            if (override_ && override_credential_.host) {
                security::erase_secret(override_credential_.secret); override_ = false;
            }
            local_->clear(); hide_passwords(); update_placeholder();
            report_status("Local connection password saved.");
        } else report_status("Could not protect or save the password for this Windows user.");
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
    hide_passwords(); peer_code_ = code; peer_->clear();
    if (!settings_ || code.size() != 8) return;
    const auto encrypted = settings_->value(peer_key()).toString().toStdString();
    if (encrypted.empty()) return;
    std::string password;
    if (security::unprotect_connection_credential(encrypted, &password)) {
        peer_->setText(QString::fromUtf8(password.data(), static_cast<int>(password.size())));
        security::erase_secret(password);
    } else report_status("Saved peer password could not be decrypted. Enter it again.");
    changed();
}
void ConnectionPasswordPanel::set_busy(bool busy) { busy_ = busy; if (busy) hide_passwords(); update_enabled(); }
bool ConnectionPasswordPanel::host_ready() const { return !saving_ && local_->text().isEmpty() && ((override_ && override_credential_.host) || !verifier_.empty()); }
bool ConnectionPasswordPanel::client_ready() const { return !saving_ && ((override_ && !override_credential_.host) || !peer_->text().isEmpty()); }
bool ConnectionPasswordPanel::prepare(bool host, security::ConnectionCredential* credential, QString* error) {
    hide_passwords();
    security::erase_secret(pending_password_); pending_code_.clear();
    if (host && !host_ready()) { *error = "Save or clear the local password before waiting."; return false; }
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
    peer_->clear(); report_status("Host rejected the connection password. Enter it again."); changed();
}
void ConnectionPasswordPanel::set_credential_override(security::ConnectionCredential credential) {
    security::erase_secret(override_credential_.secret); override_credential_ = std::move(credential); override_ = true; update_placeholder(); changed();
}
} // namespace redclaw::ui
