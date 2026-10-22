#pragma once

#include <functional>
#include <memory>

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QString>

class QWidget;

namespace redclaw::ui {

// The registry locations are injectable so OS integration tests never change
// the machine's real update policy. Production uses the machine-wide defaults.
class UpdateRestartPolicyLease final {
public:
    explicit UpdateRestartPolicyLease(
        QString policy_root = QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate"),
        QString journal_root = QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\RedClawDesktop\\UpdateRestartGuard"));
    ~UpdateRestartPolicyLease();
    bool acquire(int local_hour, QString* error);
    bool recover(QString* error);
    bool refresh(int local_hour, QString* error);
    bool release(QString* error);
    bool active() const;
    bool deadline_configured() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class HostUpdateRestartGuard final : public QObject, public QAbstractNativeEventFilter {
public:
    using Log = std::function<void(const QString&)>;
    explicit HostUpdateRestartGuard(QWidget& window, Log log,
        std::unique_ptr<UpdateRestartPolicyLease> policy = {});
    ~HostUpdateRestartGuard() override;

    void observe_session(bool host, bool running, bool connected, bool channel_open);
    void stop();
    bool nativeEventFilter(const QByteArray&, void* message, qintptr* result) override;
    static bool should_block_shutdown(bool connected_host, quint32 end_session_flags);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::ui
