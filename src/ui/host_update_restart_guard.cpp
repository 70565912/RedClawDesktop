#include "host_update_restart_guard.h"

#include <array>
#include <cmath>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTimer>
#include <QWidget>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace redclaw::ui {
namespace {
constexpr std::array<const char*, 5> kPolicyNames = {
    "SetActiveHours", "ActiveHoursStart", "ActiveHoursEnd",
    "AU/AlwaysAutoRebootAtScheduledTime", "AU/NoAutoRebootWithLoggedOnUsers"};
constexpr int kJournalVersion = 1;

bool fail(QString* error, const QString& reason) {
    if (error) *error = reason;
    return false;
}

bool elevated() {
#ifdef _WIN32
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
#else
    return false;
#endif
}

bool dword_json(const QJsonValue& value) {
    const double number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= 0
        && number <= 4294967295.0 && std::floor(number) == number;
}

bool dword_setting(const QVariant& value) {
    return value.metaType().id() == QMetaType::Int || value.metaType().id() == QMetaType::UInt
        || value.metaType().id() == QMetaType::LongLong || value.metaType().id() == QMetaType::ULongLong;
}
}  // namespace

class UpdateRestartPolicyLease::Impl {
public:
    Impl(QString policy_root, QString journal_root)
        : policy_root_(std::move(policy_root)), journal_root_(std::move(journal_root)) {}

    bool lock(QString* error) {
#ifdef _WIN32
        if (mutex_) return true;
        const auto hash = QCryptographicHash::hash(policy_root_.toUtf8(), QCryptographicHash::Sha256).toHex();
        const QString name = "Global\\RedClawDesktop.UpdateRestartGuard.v1." + QString::fromLatin1(hash.left(24));
        HANDLE mutex = CreateMutexW(nullptr, FALSE, reinterpret_cast<LPCWSTR>(name.utf16()));
        if (!mutex) return fail(error, "policy_lock_unavailable");
        const DWORD creation_status = GetLastError();
        const DWORD wait = WaitForSingleObject(mutex, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(mutex);
            return fail(error, "another_host_owns_update_protection");
        }
        // Windows mutexes are recursive on one thread. A second guard in the
        // same GUI must not recover/overwrite the first guard's live journal.
        if (wait == WAIT_OBJECT_0 && creation_status == ERROR_ALREADY_EXISTS) {
            ReleaseMutex(mutex);
            CloseHandle(mutex);
            return fail(error, "another_host_owns_update_protection");
        }
        mutex_ = mutex;
        return true;
#else
        return fail(error, "unsupported_platform");
#endif
    }

    void unlock() {
#ifdef _WIN32
        if (mutex_) { ReleaseMutex(mutex_); CloseHandle(mutex_); mutex_ = nullptr; }
#endif
    }

    bool save(QString* error) {
        QSettings journal(journal_root_, QSettings::NativeFormat);
        journal.setValue("lease", QJsonDocument(QJsonObject{
            {"schema", "redclaw.update-restart-lease.v1"}, {"version", kJournalVersion}, {"values", entries_}
        }).toJson(QJsonDocument::Compact));
        journal.sync();
        return journal.status() == QSettings::NoError || fail(error, "policy_journal_write_failed");
    }

    bool restore(QString* error) {
        QSettings journal(journal_root_, QSettings::NativeFormat);
        if (!journal.contains("lease")) return true;
        QJsonParseError parse{};
        const auto document = QJsonDocument::fromJson(journal.value("lease").toByteArray(), &parse);
        const auto object = document.object();
        const auto entries = object.value("values").toArray();
        if (parse.error != QJsonParseError::NoError || !document.isObject()
            || object.value("schema") != "redclaw.update-restart-lease.v1"
            || object.value("version").toInt(-1) != kJournalVersion || entries.size() != 5)
            return fail(error, "policy_journal_invalid");
        // Validate the complete journal before restoring any value. Never use a
        // persisted path/name to address an arbitrary registry setting.
        for (int i = 0; i < entries.size(); ++i) {
            const auto entry = entries[i].toObject();
            if (entry.value("index").toInt(-1) != i || !entry.value("present").isBool()
                || !entry.value("applied_present").isBool() || !entry.value("previous_present").isBool()
                || !dword_json(entry.value("applied")) || !dword_json(entry.value("previous"))
                || (entry.value("present").toBool() && !dword_json(entry.value("original"))))
                return fail(error, "policy_journal_invalid");
        }
        QSettings policy(policy_root_, QSettings::NativeFormat);
        for (int i = 0; i < entries.size(); ++i) {
            const auto entry = entries[i].toObject();
            const QString name = QString::fromLatin1(kPolicyNames[static_cast<std::size_t>(i)]);
            const auto current = policy.value(name);
            bool numeric = false;
            const auto number = current.toULongLong(&numeric);
            // Respect policy changes made by the administrator/MDM while the
            // lease was held, including deletion. Only undo our last writes.
            if (!policy.contains(name) || !dword_setting(current) || !numeric
                || ((!entry.value("applied_present").toBool()
                        || number != static_cast<quint64>(entry.value("applied").toDouble()))
                    && (!entry.value("previous_present").toBool()
                        || number != static_cast<quint64>(entry.value("previous").toDouble())))) continue;
            if (entry.value("present").toBool())
                policy.setValue(name, static_cast<quint32>(entry.value("original").toDouble()));
            else
                policy.remove(name);
        }
        policy.sync();
        if (policy.status() != QSettings::NoError) return fail(error, "policy_restore_failed");
        journal.remove("lease");
        journal.sync();
        return journal.status() == QSettings::NoError || fail(error, "policy_journal_cleanup_failed");
    }

    bool apply(int hour, QString* error) {
        if (hour < 0 || hour > 23) return fail(error, "invalid_local_hour");
        if (!active_) return fail(error, "policy_lease_inactive");
        // An 18-hour window containing now, refreshed on the hour. Explicitly
        // turn off the two legacy policies which override active-hours policy.
        QSettings policy(policy_root_, QSettings::NativeFormat);
        quint32 range = 18;
        if (policy.value("SetActiveHoursMaxRange", 0).toUInt() != 0) {
            const auto configured = policy.value("ActiveHoursMaxRange");
            if (!dword_setting(configured) || configured.toUInt() < 8 || configured.toUInt() > 18)
                return fail(error, "update_active_hours_range_invalid");
            range = configured.toUInt();
        }
        const std::array<quint32, 5> values = {1, static_cast<quint32>(hour),
            (static_cast<quint32>(hour) + range) % 24, 0, 0};
        bool changed = false;
        for (int i = 0; i < entries_.size(); ++i) {
            auto entry = entries_[i].toObject();
            const QString name = QString::fromLatin1(kPolicyNames[static_cast<std::size_t>(i)]);
            const bool present = policy.contains(name);
            if ((applied_ && !present)
                || (present && (!dword_setting(policy.value(name))
                    || policy.value(name).toULongLong() != static_cast<quint64>(entry.value("applied").toDouble()))))
                return fail(error, "update_policy_changed_externally");
            if (entry.value("applied").toDouble() == values[static_cast<std::size_t>(i)] && present) continue;
            changed = true;
            entry["previous"] = entry["applied"];
            entry["previous_present"] = entry["applied_present"];
            entry["applied"] = static_cast<int>(values[static_cast<std::size_t>(i)]);
            entry["applied_present"] = true;
            entries_[i] = entry;
        }
        if (applied_ && !changed) return true;
        // Persist both possible values before touching the policy. Recovery is
        // safe if a process dies between journaling and any individual write.
        if (!save(error)) return false;
        for (int i = 0; i < entries_.size(); ++i)
            policy.setValue(QString::fromLatin1(kPolicyNames[static_cast<std::size_t>(i)]),
                            values[static_cast<std::size_t>(i)]);
        policy.sync();
        if (policy.status() != QSettings::NoError) return fail(error, "update_policy_write_failed");
        QSettings readback(policy_root_, QSettings::NativeFormat);
        for (int i = 0; i < entries_.size(); ++i) {
            const QString name = QString::fromLatin1(kPolicyNames[static_cast<std::size_t>(i)]);
            if (!readback.contains(name) || readback.value(name).toUInt() != values[static_cast<std::size_t>(i)])
                return fail(error, "update_policy_readback_failed");
        }
        applied_ = true;
        return true;
    }

    QString policy_root_;
    QString journal_root_;
    QJsonArray entries_;
    bool active_ = false;
    bool applied_ = false;
#ifdef _WIN32
    HANDLE mutex_ = nullptr;
#endif
};

UpdateRestartPolicyLease::UpdateRestartPolicyLease(QString policy_root, QString journal_root)
    : impl_(std::make_unique<Impl>(std::move(policy_root), std::move(journal_root))) {}
UpdateRestartPolicyLease::~UpdateRestartPolicyLease() { QString error; release(&error); }

bool UpdateRestartPolicyLease::acquire(int hour, QString* error) {
    if (error) error->clear();
    if (impl_->active_) return refresh(hour, error);
    if (hour < 0 || hour > 23) return fail(error, "invalid_local_hour");
    if (!impl_->lock(error)) return false;
    if (!impl_->restore(error)) { impl_->unlock(); return false; }
    QSettings policy(impl_->policy_root_, QSettings::NativeFormat);
    impl_->entries_ = {};
    impl_->applied_ = false;
    for (int i = 0; i < static_cast<int>(kPolicyNames.size()); ++i) {
        const QString name = QString::fromLatin1(kPolicyNames[static_cast<std::size_t>(i)]);
        bool numeric = true;
        const bool present = policy.contains(name);
        const auto original = present ? policy.value(name).toULongLong(&numeric) : 0;
        if ((present && !dword_setting(policy.value(name))) || !numeric || original > 0xffffffffULL) {
            impl_->unlock();
            return fail(error, "update_policy_value_invalid");
        }
        impl_->entries_.append(QJsonObject{{"index", i}, {"present", present},
            {"original", static_cast<double>(original)}, {"applied", static_cast<double>(original)},
            {"previous", static_cast<double>(original)}, {"applied_present", present}, {"previous_present", present}});
    }
    impl_->active_ = true;
    if (impl_->apply(hour, error)) return true;
    const QString apply_error = error ? *error : QString();
    QString restore_error;
    release(&restore_error);
    return fail(error, restore_error.isEmpty() ? apply_error : apply_error + ";" + restore_error);
}

bool UpdateRestartPolicyLease::recover(QString* error) {
    if (error) error->clear();
    if (impl_->active_) return true;
    if (!impl_->lock(error)) return false;
    const bool restored = impl_->restore(error);
    impl_->unlock();
    return restored;
}

bool UpdateRestartPolicyLease::refresh(int hour, QString* error) { return impl_->apply(hour, error); }
bool UpdateRestartPolicyLease::release(QString* error) {
    if (error) error->clear();
    if (!impl_->active_) return true;
    const bool restored = impl_->restore(error);
    impl_->active_ = false;
    impl_->unlock();
    return restored;
}
bool UpdateRestartPolicyLease::active() const { return impl_->active_; }
bool UpdateRestartPolicyLease::deadline_configured() const {
    QSettings policy(impl_->policy_root_, QSettings::NativeFormat);
    return policy.value("SetComplianceDeadline", 0).toUInt() != 0
        || policy.value("SetAutoRestartDeadline", 0).toUInt() != 0;
}

class HostUpdateRestartGuard::Impl {
public:
    Impl(QWidget& window, Log log, std::unique_ptr<UpdateRestartPolicyLease> policy)
        : window_(window), log_(std::move(log)),
          policy_(policy ? std::move(policy) : std::make_unique<UpdateRestartPolicyLease>()) {}

    void refresh() {
        if (!connected_) return;
        QString error;
        const int hour = QDateTime::currentDateTime().time().hour();
        if (elevated()) {
            const bool ok = policy_->active() ? policy_->refresh(hour, &error) : policy_->acquire(hour, &error);
            report(ok ? (policy_->deadline_configured() ? "active_deadline_may_override" : "active") : error);
        } else {
            report("administrator_required");
        }
    }

    void report(const QString& state) {
        if (last_report_ == state) return;
        last_report_ = state;
        if (log_) log_("Host update restart protection state=" + state);
    }

    QWidget& window_;
    Log log_;
    std::unique_ptr<UpdateRestartPolicyLease> policy_;
    QTimer timer_;
    bool connected_ = false;
    bool shutdown_reason_ = false;
    QString last_report_;
};

HostUpdateRestartGuard::HostUpdateRestartGuard(QWidget& window, Log log,
    std::unique_ptr<UpdateRestartPolicyLease> policy)
    : QObject(&window), impl_(std::make_unique<Impl>(window, std::move(log), std::move(policy))) {
    QCoreApplication::instance()->installNativeEventFilter(this);
    if (elevated()) {
        QString error;
        if (!impl_->policy_->recover(&error)) impl_->report(error);
    }
    impl_->timer_.setInterval(60 * 1000);
    QObject::connect(&impl_->timer_, &QTimer::timeout, this, [this] { impl_->refresh(); });
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] { stop(); });
}

HostUpdateRestartGuard::~HostUpdateRestartGuard() {
    stop();
    QCoreApplication::instance()->removeNativeEventFilter(this);
}

void HostUpdateRestartGuard::observe_session(bool host, bool running, bool connected, bool channel_open) {
    const bool protect = host && running && connected && channel_open;
    if (protect == impl_->connected_) return;
    if (!protect) { stop(); return; }
    impl_->connected_ = true;
#ifdef _WIN32
    impl_->shutdown_reason_ = ShutdownBlockReasonCreate(
        reinterpret_cast<HWND>(impl_->window_.winId()), L"RedClaw: a remote desktop session is connected.") != FALSE;
    if (!impl_->shutdown_reason_) impl_->report("shutdown_reason_registration_failed");
#endif
    impl_->refresh();
    impl_->timer_.start();
}

void HostUpdateRestartGuard::stop() {
    if (!impl_->connected_) return;
    impl_->connected_ = false;
    impl_->timer_.stop();
#ifdef _WIN32
    if (impl_->shutdown_reason_) ShutdownBlockReasonDestroy(reinterpret_cast<HWND>(impl_->window_.winId()));
#endif
    impl_->shutdown_reason_ = false;
    QString error;
    const bool restored = impl_->policy_->release(&error);
    impl_->report(restored ? "released" : error);
}

bool HostUpdateRestartGuard::should_block_shutdown(bool connected_host, quint32 flags) {
    constexpr quint32 kCritical = 0x40000000;
    constexpr quint32 kLogoff = 0x80000000;
    return connected_host && (flags & (kCritical | kLogoff)) == 0;
}

bool HostUpdateRestartGuard::nativeEventFilter(const QByteArray& type, void* message, qintptr* result) {
#ifdef _WIN32
    if ((type == "windows_generic_MSG" || type == "windows_dispatcher_MSG") && message) {
        const auto* msg = static_cast<MSG*>(message);
        if (impl_->connected_ && msg->message == WM_QUERYENDSESSION
            && msg->hwnd == reinterpret_cast<HWND>(impl_->window_.winId())
            && should_block_shutdown(impl_->connected_, static_cast<quint32>(msg->lParam))) {
            *result = FALSE;
            if (impl_->log_) impl_->log_("Host update restart protection blocked cancelable shutdown; remote session remains connected");
            return true;
        }
    }
#else
    Q_UNUSED(type); Q_UNUSED(message); Q_UNUSED(result);
#endif
    return false;
}

}  // namespace redclaw::ui
