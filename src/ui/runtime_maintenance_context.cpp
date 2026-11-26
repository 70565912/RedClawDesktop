#include "ui/runtime_maintenance_context.h"
#include "ui/windows_user_security.h"
#include "ui/gui_startup_options.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUuid>
#include <memory>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Aclapi.h>
#include <sddl.h>
#endif

namespace redclaw::ui {
namespace {
bool fail(QString* error, const char* reason) { if (error) *error = QString::fromLatin1(reason); return false; }
#ifdef _WIN32
QString legacy_video_suffix(runtime::DesktopVideoMode mode, bool has_profile) {
    if (mode == runtime::DesktopVideoMode::kDesktop) return "--stream-smoke";
    if (mode == runtime::DesktopVideoMode::kDisabled && !has_profile) return {};
    return desktop_video_worker_arguments(mode).front();
}
using win::File;
using win::Local;
using win::UserSecurity;
quint64 started_ms(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    const auto ticks = (static_cast<quint64>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    return ticks >= 116444736000000000ULL ? (ticks - 116444736000000000ULL) / 10000 : 0;
}
bool private_file(HANDLE file, PSID user) { return win::owner_only_object(file, user, false); }
#endif
bool read_arguments(const QJsonValue& value, QStringList& output) {
    if (!value.isArray()) return false;
    for (const auto& item : value.toArray()) {
        if (!item.isString() || item.toString().contains(QChar('\0'))) return false;
        output.push_back(item.toString());
    }
    return !output.isEmpty();
}
}
bool RuntimeMaintenanceSnapshot::declares_agent_projects() const {
    for (qsizetype i = 0; i + 1 < runtime_arguments.size(); ++i) {
        if (runtime_arguments[i] != "--agent-project-root" && runtime_arguments[i] != "--agent-project-manifest") continue;
        const auto value = runtime_arguments[i + 1].trimmed();
        if (!value.isEmpty() && !value.startsWith("--")) return true;
    }
    return false;
}
bool RuntimeMaintenanceContext::prepare(QString* error) {
    path_.clear();
#ifdef _WIN32
    UserSecurity security;
    if (!security.load()) return fail(error, "maintenance_context_user_unavailable");
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/maintenance";
    if (!QDir().mkpath(base)) return fail(error, "maintenance_context_directory_unavailable");
    const auto directory = QDir::toNativeSeparators(base + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!CreateDirectoryW(reinterpret_cast<LPCWSTR>(directory.utf16()), &security.attributes))
        return fail(error, "maintenance_context_directory_create_failed");
    path_ = QDir::toNativeSeparators(directory + "/session.json");
    if (error) error->clear(); return true;
#else
    return fail(error, "maintenance_platform_unsupported");
#endif
}
bool RuntimeMaintenanceContext::save(const QStringList& gui_arguments, const QStringList& runtime_arguments,
    quint64 runtime_pid, QString* error) {
#ifdef _WIN32
    if (path_.isEmpty() || gui_arguments.isEmpty() || runtime_arguments.isEmpty() || runtime_pid > MAXDWORD)
        return fail(error, "maintenance_context_invalid");
    File runtime(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(runtime_pid)));
    const auto runtime_started = runtime ? started_ms(runtime.get()) : 0;
    const auto gui_started = started_ms(GetCurrentProcess());
    if (!runtime_started || !gui_started) return fail(error, "maintenance_context_identity_unavailable");
    auto legacy_arguments = runtime_arguments;
    const auto video_flag = legacy_arguments.takeLast();
    const auto mode = runtime::desktop_video_flag(video_flag.toStdString());
    if (!mode) return fail(error, "maintenance_video_mode_missing");
    const auto suffix = legacy_video_suffix(*mode, legacy_arguments.contains("--runtime-config"));
    if (!suffix.isEmpty()) legacy_arguments << suffix;
    QJsonObject context{{"schema", "redclaw.runtime-maintenance.context.v1"}, {"role", "host"},
        {"gui_pid", static_cast<qint64>(QCoreApplication::applicationPid())}, {"gui_started_ms", static_cast<qint64>(gui_started)},
        {"runtime_pid", static_cast<qint64>(runtime_pid)}, {"runtime_started_ms", static_cast<qint64>(runtime_started)},
        {"executable", QDir::toNativeSeparators(QCoreApplication::applicationFilePath())},
        {"working_directory", QDir::toNativeSeparators(QDir::currentPath())},
        {"previous_context", previous_context_}, {"video_arguments_version", 2}, {"worker_video_flag", video_flag},
        {"gui_arguments", QJsonArray::fromStringList(gui_arguments)}, {"runtime_arguments", QJsonArray::fromStringList(legacy_arguments)}};
    const auto bytes = QJsonDocument(context).toJson(QJsonDocument::Compact);
    UserSecurity security;
    if (!security.load()) return fail(error, "maintenance_context_user_unavailable");
    const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path_.utf16()), GENERIC_WRITE, 0,
        &security.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return fail(error, "maintenance_context_create_failed");
    File file(handle); DWORD written = 0;
    if (!WriteFile(file.get(), bytes.constData(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        || written != static_cast<DWORD>(bytes.size()) || !FlushFileBuffers(file.get()))
        return fail(error, "maintenance_context_write_failed");
    if (error) error->clear(); return true;
#else
    (void)gui_arguments; (void)runtime_arguments; (void)runtime_pid;
    return fail(error, "maintenance_platform_unsupported");
#endif
}
std::optional<RuntimeMaintenanceSnapshot> RuntimeMaintenanceContext::read(const QString& path, QString* error) {
#ifdef _WIN32
    UserSecurity security;
    if (!security.load()) { fail(error, "maintenance_context_user_unavailable"); return {}; }
    const auto native = QDir::toNativeSeparators(path);
    const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(native.utf16()), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { fail(error, "maintenance_context_open_failed"); return {}; }
    File file(handle); LARGE_INTEGER length{};
    if (!private_file(file.get(), security.sid()) || !GetFileSizeEx(file.get(), &length)
        || length.QuadPart <= 0 || length.QuadPart > 1024 * 1024) { fail(error, "maintenance_context_not_private_or_invalid"); return {}; }
    QByteArray bytes(static_cast<qsizetype>(length.QuadPart), '\0'); DWORD received = 0;
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &received, nullptr)
        || received != static_cast<DWORD>(bytes.size())) { fail(error, "maintenance_context_read_failed"); return {}; }
    const auto document = QJsonDocument::fromJson(bytes);
    const auto context = document.object(); RuntimeMaintenanceSnapshot snapshot;
    if (!document.isObject() || context.value("schema").toString() != "redclaw.runtime-maintenance.context.v1"
        || context.value("role").toString() != "host"
        || QString::compare(QDir::cleanPath(QDir::fromNativeSeparators(context.value("executable").toString())),
            QCoreApplication::applicationFilePath(), Qt::CaseInsensitive) != 0
        || !read_arguments(context.value("gui_arguments"), snapshot.gui_arguments)
        || !read_arguments(context.value("runtime_arguments"), snapshot.runtime_arguments)
        || snapshot.gui_arguments.contains("--gui-maintenance-resume") || snapshot.runtime_arguments.contains("--gui-runtime-stdio")) {
        fail(error, "maintenance_context_invalid"); return {};
    }
    const auto video_version = context.value("video_arguments_version");
    if (video_version.isUndefined()) {
        if (!normalize_saved_video_arguments(snapshot.runtime_arguments, error)) return {};
    } else if (video_version.toInt(-1) != 2) {
        fail(error, "maintenance_video_version_incompatible"); return {};
    } else {
        const auto flag = context.value("worker_video_flag").toString();
        const auto mode = runtime::desktop_video_flag(flag.toStdString());
        if (!mode) { fail(error, "maintenance_video_mode_missing"); return {}; }
        const auto suffix = legacy_video_suffix(*mode, snapshot.runtime_arguments.contains("--runtime-config"));
        if (!suffix.isEmpty() && (snapshot.runtime_arguments.isEmpty() || snapshot.runtime_arguments.takeLast() != suffix)) {
            fail(error, "maintenance_video_arguments_invalid"); return {};
        }
        snapshot.runtime_arguments << flag;
    }
    if (error) error->clear(); return snapshot;
#else
    (void)path; fail(error, "maintenance_platform_unsupported"); return {};
#endif
}
}
