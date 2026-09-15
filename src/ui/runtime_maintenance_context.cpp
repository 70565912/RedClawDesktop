#include "ui/runtime_maintenance_context.h"
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
struct CloseFile { void operator()(void* value) const { CloseHandle(value); } };
struct FreeLocal { void operator()(void* value) const { LocalFree(value); } };
using File = std::unique_ptr<void, CloseFile>;
using Local = std::unique_ptr<void, FreeLocal>;
struct UserSecurity {
    std::vector<unsigned char> token_user;
    Local descriptor;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    PSID sid() const { return reinterpret_cast<const TOKEN_USER*>(token_user.data())->User.Sid; }
    bool load() {
        HANDLE value = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &value)) return false;
        File token(value); DWORD bytes = 0;
        (void)GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
        if (!bytes) return false;
        token_user.resize(bytes);
        if (!GetTokenInformation(token.get(), TokenUser, token_user.data(), bytes, &bytes)) return false;
        LPWSTR name = nullptr;
        if (!ConvertSidToStringSidW(sid(), &name)) return false;
        Local owner(name);
        const std::wstring sddl = L"O:" + std::wstring(name) + L"D:P(A;OICI;FA;;;" + name + L")";
        PSECURITY_DESCRIPTOR security = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security, nullptr)) return false;
        descriptor.reset(security); attributes.lpSecurityDescriptor = security; return true;
    }
};
quint64 started_ms(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    const auto ticks = (static_cast<quint64>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    return ticks >= 116444736000000000ULL ? (ticks - 116444736000000000ULL) / 10000 : 0;
}
bool private_file(HANDLE file, PSID current_user) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof(info))
        || (info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return false;
    PSID owner = nullptr; PACL acl = nullptr; PSECURITY_DESCRIPTOR security = nullptr;
    if (GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &security) != ERROR_SUCCESS) return false;
    Local descriptor(security);
    SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
    if (!owner || !EqualSid(owner, current_user) || !acl || !GetSecurityDescriptorControl(security, &control, &revision)
        || !(control & SE_DACL_PROTECTED)) return false;
    bool allowed = false;
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* raw = nullptr;
        if (!GetAce(acl, index, &raw)) return false;
        const auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        if (!EqualSid(&ace->SidStart, current_user)) return false;
        allowed = true;
    }
    return allowed;
}
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
    QJsonObject context{{"schema", "redclaw.runtime-maintenance.context.v1"}, {"role", "host"},
        {"gui_pid", static_cast<qint64>(QCoreApplication::applicationPid())}, {"gui_started_ms", static_cast<qint64>(gui_started)},
        {"runtime_pid", static_cast<qint64>(runtime_pid)}, {"runtime_started_ms", static_cast<qint64>(runtime_started)},
        {"executable", QDir::toNativeSeparators(QCoreApplication::applicationFilePath())},
        {"working_directory", QDir::toNativeSeparators(QDir::currentPath())},
        {"previous_context", previous_context_},
        {"gui_arguments", QJsonArray::fromStringList(gui_arguments)}, {"runtime_arguments", QJsonArray::fromStringList(runtime_arguments)}};
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
    if (error) error->clear(); return snapshot;
#else
    (void)path; fail(error, "maintenance_platform_unsupported"); return {};
#endif
}
}
