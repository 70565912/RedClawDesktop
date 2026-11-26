#include "ui/private_gui_profile.h"
#include "redclaw/security/connection_auth.h"
#include "ui/windows_user_security.h"
#include <QDir>
#include <QGuiApplication>
#include <QMainWindow>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#ifdef _WIN32
#include <ShlObj.h>
#endif

namespace redclaw::ui {
bool isolate_private_qt_logging() {
#if defined(_WIN32) && defined(_DEBUG)
    if (QCoreApplication::instance()) return false;
    QStandardPaths::setTestModeEnabled(true);
    for (const char* name : {"QT_LOGGING_CONF", "QT_LOGGING_RULES"}) {
        if (qEnvironmentVariableIsSet(name) && !qunsetenv(name)) return false;
    }
    return QStandardPaths::locate(QStandardPaths::GenericConfigLocation, "QtProject/qtlogging.ini").isEmpty();
#else
    return false;
#endif
}

QStringList private_gui_worker_arguments(const GuiAutoStartOptions& gui) {
    if (gui.private_profile_dir.isEmpty()) return {};
    return {"--disable-ipv6-candidates", "--no-system-clipboard", "--run-seconds", QString::number(gui.run_seconds),
        "--signal-timeout-seconds", QString::number(gui.signal_timeout_seconds)};
}

bool prepare_private_gui_profile(const runtime::RuntimeOptions& args, GuiAutoStartOptions& gui) {
#if defined(_WIN32) && defined(_DEBUG)
    if (args.run_seconds > runtime::kMaxRuntimeSeconds || args.signal_timeout_seconds > runtime::kMaxRuntimeSeconds) return false;
    const auto path = std::filesystem::path(args.gui_test_profile);
    const auto leaf = QString::fromStdWString(path.filename().wstring());
    static const QRegularExpression name(
        R"(^RedClawGuiTest-[a-fA-F0-9]{8}(?:-[a-fA-F0-9]{4}){3}-[a-fA-F0-9]{12}$)");
    if (!name.match(leaf).hasMatch() || !path.is_absolute()) return false;
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &local))) return false;
    const std::filesystem::path parent(local);
    CoTaskMemFree(local);
    // Match the exact known parent, excluding device paths, traversal, ADS and aliases.
    if (path.parent_path() != parent || GetDriveTypeW(parent.root_path().c_str()) != DRIVE_FIXED) return false;
    for (auto dir = parent; dir != dir.root_path(); dir = dir.parent_path()) {
        const auto attributes = GetFileAttributesW(dir.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    win::UserSecurity security;
    if (!security.load(true) || !CreateDirectoryW(path.c_str(), &security.attributes)) return false;
    HANDLE handle = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    win::File directory(handle);
    if (!win::owner_only_object(handle, security.sid(), true)) return false;
    const QDir root(QString::fromStdWString(path.wstring()));
    gui = {};
    gui.private_profile_dir = root.absolutePath();
    gui.role = "controller";
    gui.transport = "file";
    gui.signal_dir = root.filePath("signal");
    gui.network_bind_address = "127.0.0.1";
    gui.ice_udp_port = args.ice_udp_port;
    gui.ice_udp_port_explicit = true;
    gui.signal_timeout_seconds = static_cast<int>(args.signal_timeout_seconds);
    gui.run_seconds = static_cast<int>(args.run_seconds);
    if (args.video_mode != runtime::DesktopVideoMode::kDesktop) gui.video_override = args.video_mode;
    gui.log_dir = root.filePath("logs");
    gui.coordination_journal_path = root.filePath("coordination.jsonl");
    gui.debug_control_name = leaf + ".debug";
    gui.agent_control_name = leaf + ".agent";
    gui.workspace_control_name = leaf + ".workspace";
    gui.enable_debug_control = true;
    gui.enable_workspace_control = false;
    return true;
#else
    (void)args; (void)gui;
    return false;
#endif
}

QSettings* open_gui_settings(const GuiAutoStartOptions& gui, const QString& log_dir, QObject* parent) {
    if (!gui.private_profile_dir.isEmpty()) {
        auto* settings = new QSettings(QDir(gui.private_profile_dir).filePath("ui.ini"), QSettings::IniFormat, parent);
        settings->setFallbacksEnabled(false);
        return settings;
    }
    if (gui.agent_qa_fixture_provider)
        return new QSettings(QDir(log_dir).filePath("qa-ui-settings.ini"), QSettings::IniFormat, parent);
    return new QSettings("RedClaw", "RedClawDesktop", parent);
}

bool load_private_controller_credential(const QString& root, security::ConnectionCredential& result) {
#if defined(_WIN32) && defined(_DEBUG)
    if (root.isEmpty()) return false;
    const auto path = std::filesystem::path(root.toStdWString()) / L"controller.dpapi";
    // Pin the ordinary file against replacement or writing while the DPAPI loader opens it.
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    win::File file(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || info.nNumberOfLinks != 1 ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    if (!security::load_connection_credential_file(path, &result)) return false;
    if (!result.host) return true;
    security::erase_secret(result.secret);
#else
    (void)root; (void)result;
#endif
    return false;
}

#if defined(_WIN32) && defined(_DEBUG)
namespace {
class PrivateWindow final : public QMainWindow {
    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override {
        auto* msg = static_cast<MSG*>(message);
        if (msg->message == WM_WINDOWPOSCHANGING) {
            reinterpret_cast<WINDOWPOS*>(msg->lParam)->flags |= SWP_NOZORDER;
        } else if (msg->message == WM_STYLECHANGING && static_cast<int>(msg->wParam) == GWL_EXSTYLE) {
            reinterpret_cast<STYLESTRUCT*>(msg->lParam)->styleNew |= WS_EX_NOACTIVATE;
        } else if (msg->message == WM_MOUSEACTIVATE) {
            *result = MA_NOACTIVATEANDEAT;
            return true;
        }
        return QMainWindow::nativeEvent(type, message, result);
    }
};

bool below_window(HWND hwnd, HWND other) {
    for (auto above = GetWindow(hwnd, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV)) {
        if (above == other) return true;
    }
    return false;
}

bool inactive_private_window(HWND hwnd) {
    const auto style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    return (style & (WS_EX_NOACTIVATE | WS_EX_TOPMOST)) == WS_EX_NOACTIVATE &&
        hwnd != GetForegroundWindow() && hwnd != GetActiveWindow();
}
}
#endif

std::unique_ptr<QMainWindow> create_gui_window(bool private_presenter) {
    if (!private_presenter) return std::make_unique<QMainWindow>();
#if defined(_WIN32) && defined(_DEBUG)
    if (QGuiApplication::platformName() != "windows") return {};
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    GetStartupInfoW(&startup);
    // Windows can substitute the launcher's show command on the first ShowWindow.
    if ((startup.dwFlags & STARTF_USESHOWWINDOW) &&
        startup.wShowWindow != SW_SHOWNOACTIVATE && startup.wShowWindow != SW_SHOWNA) return {};
    auto window = std::make_unique<PrivateWindow>();
    window->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                          Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
    window->setAttribute(Qt::WA_ShowWithoutActivating);
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!IsWindow(hwnd) || IsWindowVisible(hwnd)) return {};
    const auto style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style | WS_EX_NOACTIVATE);
    if (!SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOZORDER |
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER) ||
        !inactive_private_window(hwnd)) return {};
    return window;
#else
    return {};
#endif
}

#if defined(_WIN32) && defined(_DEBUG)
bool show_private_gui_window_behind(QMainWindow& window, WId foreground_id) {
    const auto hwnd = reinterpret_cast<HWND>(window.winId());
    const auto foreground = reinterpret_cast<HWND>(foreground_id);
    const auto owner = GetAncestor(foreground, GA_ROOTOWNER);
    if (!owner || hwnd == owner || IsWindowVisible(hwnd) || !inactive_private_window(hwnd)) return false;

    // Only lower the hidden presenter. Bypass its Z-order lock for this one placement.
    // An owned foreground dialog and its owner both stay above the presenter.
    if (!below_window(hwnd, owner) &&
        !SetWindowPos(hwnd, owner, 0, 0, 0, 0, SWP_NOSENDCHANGING | SWP_NOMOVE |
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) return false;
    const auto safe_position = [&] {
        const HWND current = GetForegroundWindow();
        return inactive_private_window(hwnd) && below_window(hwnd, foreground) &&
            (owner == foreground || below_window(hwnd, owner)) &&
            (!current || current == foreground || below_window(hwnd, current));
    };
    if (!safe_position()) return false;

    window.show();
    if (reinterpret_cast<HWND>(window.internalWinId()) == hwnd && IsWindowVisible(hwnd) && safe_position()) return true;
    window.hide();
    return false;
}
#endif

bool show_gui_window(QMainWindow& window, bool private_presenter) {
    if (private_presenter) {
#if defined(_WIN32) && defined(_DEBUG)
        return show_private_gui_window_behind(window, reinterpret_cast<WId>(GetForegroundWindow()));
#else
        return false;
#endif
    }
    window.show();
    return true;
}
}
