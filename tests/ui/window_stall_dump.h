#pragma once
#if defined(_WIN32)
#include <Windows.h>
#include <DbgHelp.h>
#include <atomic>
#include <thread>
#include <QByteArray>
#pragma comment(lib, "Dbghelp.lib")

// Opt-in local diagnostic only. The dump captures the blocked GUI thread; it
// is never enabled in performance runs and does not change tracing policies.
class WindowStallDump {
public:
    WindowStallDump() : path_(qgetenv("REDCLAW_UI_STALL_DUMP")), gui_thread_(GetCurrentThreadId()) {
        if (path_.isEmpty()) return;
        worker_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                const auto begin = begin_.load();
                if (begin && GetTickCount64() - begin >= 100) {
                    const auto file = CreateFileA(path_.constData(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (file != INVALID_HANDLE_VALUE) {
                        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpWithThreadInfo, nullptr, nullptr, nullptr);
                        CloseHandle(file);
                        std::fprintf(stderr, "UI stall dump gui_thread=%lu path=%s\n", gui_thread_, path_.constData());
                    }
                    return;
                }
                Sleep(5);
            }
        });
    }
    void begin() { begin_ = GetTickCount64(); }
    void end() { begin_ = 0; }
private:
    QByteArray path_;
    DWORD gui_thread_;
    std::atomic<ULONGLONG> begin_{0};
    std::jthread worker_;
};
#else
class WindowStallDump { public: void begin() {} void end() {} };
#endif
