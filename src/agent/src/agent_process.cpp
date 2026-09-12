#include "redclaw/agent/agent_providers.h"
#include "owned_agent_job.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace redclaw::agent {
namespace {

void assign_error(std::string value, std::string* error) {
    if (error != nullptr) {
        *error = std::move(value);
    }
}

#ifdef _WIN32

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string wide_to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring quote_argument(const std::wstring& value) {
    std::wstring quoted = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'\"') {
            quoted.append(slashes * 2U + 1U, L'\\');
            quoted.push_back(ch);
            slashes = 0;
            continue;
        }
        quoted.append(slashes, L'\\');
        slashes = 0;
        quoted.push_back(ch);
    }
    quoted.append(slashes * 2U, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

enum class WindowsLaunchKind {
    kExecutable,
    kPowerShellScript,
};

struct ResolvedWindowsLaunch {
    WindowsLaunchKind kind = WindowsLaunchKind::kExecutable;
    std::wstring target_path;
    std::wstring application_path;
    std::wstring command_line;
};

std::optional<std::wstring> search_windows_path(
    const std::wstring& executable,
    const wchar_t* extension) {
    const DWORD required = SearchPathW(
        nullptr, executable.c_str(), extension, 0, nullptr, nullptr);
    if (required != 0) {
        std::vector<wchar_t> path(required + 1U);
        if (SearchPathW(nullptr, executable.c_str(), extension,
                static_cast<DWORD>(path.size()), path.data(), nullptr) != 0) {
            return std::wstring(path.data());
        }
    }

    DWORD bytes = 0;
    const LSTATUS size_status = RegGetValueW(
        HKEY_CURRENT_USER, L"Environment", L"Path",
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, &bytes);
    if (size_status != ERROR_SUCCESS || bytes <= sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::vector<wchar_t> raw(bytes / sizeof(wchar_t) + 1U, L'\0');
    if (RegGetValueW(
            HKEY_CURRENT_USER, L"Environment", L"Path",
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, raw.data(), &bytes)
        != ERROR_SUCCESS) {
        return std::nullopt;
    }
    const DWORD expanded_size = ExpandEnvironmentStringsW(raw.data(), nullptr, 0);
    if (expanded_size == 0) {
        return std::nullopt;
    }
    std::vector<wchar_t> expanded(expanded_size, L'\0');
    if (ExpandEnvironmentStringsW(
            raw.data(), expanded.data(), static_cast<DWORD>(expanded.size())) == 0) {
        return std::nullopt;
    }
    std::wstring file_name = executable;
    if (extension != nullptr) {
        const std::size_t slash = file_name.find_last_of(L"\\/");
        const std::size_t dot = file_name.find_last_of(L'.');
        if (dot == std::wstring::npos
            || (slash != std::wstring::npos && dot < slash)) {
            file_name += extension;
        }
    }
    const std::wstring user_path(expanded.data());
    std::size_t start = 0;
    while (start <= user_path.size()) {
        const std::size_t separator = user_path.find(L';', start);
        std::wstring directory = user_path.substr(
            start, separator == std::wstring::npos ? std::wstring::npos : separator - start);
        if (directory.size() >= 2U && directory.front() == L'"'
            && directory.back() == L'"') {
            directory = directory.substr(1U, directory.size() - 2U);
        }
        if (!directory.empty()) {
            const std::wstring candidate = directory
                + (directory.back() == L'\\' || directory.back() == L'/' ? L"" : L"\\")
                + file_name;
            const DWORD attributes = GetFileAttributesW(candidate.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES
                && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                return candidate;
            }
        }
        if (separator == std::wstring::npos) {
            break;
        }
        start = separator + 1U;
    }
    return std::nullopt;
}

std::optional<std::wstring> search_codex_desktop_cli() {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }
    std::vector<wchar_t> local_app_data(required, L'\0');
    if (GetEnvironmentVariableW(
            L"LOCALAPPDATA", local_app_data.data(),
            static_cast<DWORD>(local_app_data.size())) == 0) {
        return std::nullopt;
    }

    const std::filesystem::path bin_root = std::filesystem::path(local_app_data.data())
        / L"OpenAI" / L"Codex" / L"bin";
    std::error_code error;
    if (!std::filesystem::is_directory(bin_root, error)) {
        return std::nullopt;
    }

    std::optional<std::filesystem::path> newest;
    std::filesystem::file_time_type newest_time{};
    for (std::filesystem::directory_iterator entries(
             bin_root, std::filesystem::directory_options::skip_permission_denied, error);
         !error && entries != std::filesystem::directory_iterator();
         entries.increment(error)) {
        const auto candidate = entries->path() / L"codex.exe";
        if (!std::filesystem::is_regular_file(candidate, error)) {
            error.clear();
            continue;
        }
        const auto write_time = std::filesystem::last_write_time(candidate, error);
        if (error) {
            error.clear();
            continue;
        }
        if (!newest || write_time > newest_time) {
            newest = candidate;
            newest_time = write_time;
        }
    }
    return newest ? std::optional<std::wstring>(newest->wstring()) : std::nullopt;
}

std::wstring lowercase_extension(const std::wstring& path) {
    const std::size_t separator = path.find_last_of(L"\\/");
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos
        || (separator != std::wstring::npos && dot < separator)) {
        return {};
    }
    std::wstring extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return extension;
}

bool resolve_windows_launch(
    const std::string& executable,
    const std::vector<std::string>& arguments,
    ResolvedWindowsLaunch* launch,
    std::string* error) {
    if (launch == nullptr) {
        assign_error("agent launch output must be non-null", error);
        return false;
    }
    const std::wstring requested = utf8_to_wide(executable);
    if (requested.empty()) {
        assign_error("agent executable name is invalid UTF-8 or empty", error);
        return false;
    }

    const std::wstring requested_extension = lowercase_extension(requested);
    std::optional<std::wstring> target;
    WindowsLaunchKind kind = WindowsLaunchKind::kExecutable;
    if (requested_extension.empty() || requested_extension == L".exe") {
        target = search_windows_path(
            requested, requested_extension.empty() ? L".exe" : nullptr);
    }
    std::wstring requested_name = requested;
    std::transform(requested_name.begin(), requested_name.end(), requested_name.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    if (!target && requested_extension.empty() && requested_name == L"codex") {
        target = search_codex_desktop_cli();
    }
    if (!target && (requested_extension.empty() || requested_extension == L".ps1")) {
        target = search_windows_path(
            requested, requested_extension.empty() ? L".ps1" : nullptr);
        kind = WindowsLaunchKind::kPowerShellScript;
    }
    if (!target) {
        assign_error("agent executable or PowerShell launcher was not found", error);
        return false;
    }

    std::wstring application_path = *target;
    std::wstring command;
    if (kind == WindowsLaunchKind::kPowerShellScript) {
        const auto powershell = search_windows_path(L"powershell.exe", nullptr);
        if (!powershell) {
            assign_error("Windows PowerShell is unavailable for the agent launcher", error);
            return false;
        }
        application_path = *powershell;
        command = quote_argument(application_path)
            + L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "
            + quote_argument(*target);
    } else {
        command = quote_argument(application_path);
    }
    for (const auto& argument : arguments) {
        const std::wstring wide_argument = utf8_to_wide(argument);
        if (!argument.empty() && wide_argument.empty()) {
            assign_error("agent argument contains invalid UTF-8", error);
            return false;
        }
        command.push_back(L' ');
        command += quote_argument(wide_argument);
    }
    *launch = {
        .kind = kind,
        .target_path = std::move(*target),
        .application_path = std::move(application_path),
        .command_line = std::move(command),
    };
    return true;
}

class WindowsAgentProcess final : public IAgentProcess {
public:
    ~WindowsAgentProcess() override {
        stop();
    }

    bool executable_available(const std::string& executable, std::string* version) override {
        ResolvedWindowsLaunch launch;
        if (!resolve_windows_launch(executable, {}, &launch, nullptr)) {
            return false;
        }
        if (version != nullptr) {
            *version = wide_to_utf8(launch.target_path);
        }
        return true;
    }

    bool run_probe(
        const std::string& executable,
        const std::vector<std::string>& arguments,
        std::uint32_t timeout_ms,
        int* exit_code,
        std::string* output,
        std::string* error) override {
        if (exit_code == nullptr || output == nullptr) {
            assign_error("probe outputs must be non-null", error);
            return false;
        }
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE read_handle = nullptr;
        HANDLE write_handle = nullptr;
        if (CreatePipe(&read_handle, &write_handle, &security, 0) == FALSE) {
            assign_error("failed to create agent probe pipe", error);
            return false;
        }
        SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);
        ResolvedWindowsLaunch launch;
        if (!resolve_windows_launch(executable, arguments, &launch, error)) {
            CloseHandle(read_handle);
            CloseHandle(write_handle);
            return false;
        }
        std::vector<wchar_t> mutable_command(
            launch.command_line.begin(), launch.command_line.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = write_handle;
        startup.hStdError = write_handle;
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            launch.application_path.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, nullptr, nullptr,
            &startup, &process);
        CloseHandle(write_handle);
        if (created == FALSE) {
            CloseHandle(read_handle);
            assign_error("failed to start agent readiness probe", error);
            return false;
        }
        OwnedAgentJob probe_job;
        const bool contained = probe_job.attach_and_resume(process);
        CloseHandle(process.hThread);
        if (!contained) {
            CloseHandle(read_handle);
            CloseHandle(process.hProcess);
            assign_error("cannot isolate agent readiness probe", error);
            return false;
        }
        output->clear();
        std::array<char, 4096> buffer{};
        const ULONGLONG started_at = GetTickCount64();
        bool timed_out = false;
        for (;;) {
            if (GetTickCount64() - started_at >= (timeout_ms == 0 ? 60000 : timeout_ms)) {
                timed_out = true;
                probe_job.terminate(124);
                WaitForSingleObject(process.hProcess, 2000);
                break;
            }
            DWORD available = 0;
            if (PeekNamedPipe(read_handle, nullptr, 0, nullptr, &available, nullptr) != FALSE
                && available > 0) {
                DWORD read = 0;
                const DWORD requested = (std::min)(
                    available, static_cast<DWORD>(buffer.size()));
                if (ReadFile(read_handle, buffer.data(), requested, &read, nullptr) != FALSE
                    && read > 0 && output->size() < 64U * 1024U) {
                    const std::size_t remaining = 64U * 1024U - output->size();
                    output->append(buffer.data(), std::min<std::size_t>(read, remaining));
                }
                continue;
            }
            const DWORD wait = WaitForSingleObject(process.hProcess, 10);
            if (wait == WAIT_OBJECT_0) {
                break;
            }
            if (wait == WAIT_FAILED
                || (timeout_ms != 0 && GetTickCount64() - started_at >= timeout_ms)) {
                timed_out = true;
                probe_job.terminate(124);
                WaitForSingleObject(process.hProcess, 2000);
                break;
            }
        }
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess);
        probe_job.reset();
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(read_handle, nullptr, 0, nullptr, &available, nullptr)
                || available == 0 || output->size() >= 64U * 1024U) break;
            DWORD read = 0;
            if (ReadFile(read_handle, buffer.data(), (std::min)(available, static_cast<DWORD>(buffer.size())),
                    &read, nullptr) == FALSE || read == 0) {
                break;
            }
            if (output->size() < 64U * 1024U) {
                const std::size_t remaining = 64U * 1024U - output->size();
                output->append(buffer.data(), std::min<std::size_t>(read, remaining));
            }
        }
        CloseHandle(read_handle);
        *exit_code = static_cast<int>(code);
        if (timed_out) {
            assign_error("agent readiness probe timed out", error);
            return false;
        }
        return true;
    }

    bool start(
        const std::string& executable,
        const std::vector<std::string>& arguments,
        const std::filesystem::path& working_directory,
        AgentProcessLineCallback stdout_line,
        AgentProcessLineCallback stderr_line,
        AgentProcessExitCallback exited,
        std::string* error) override {
        stop();
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE child_stdout_read = nullptr;
        HANDLE child_stdout_write = nullptr;
        HANDLE child_stderr_read = nullptr;
        HANDLE child_stderr_write = nullptr;
        HANDLE child_stdin_read = nullptr;
        HANDLE child_stdin_write = nullptr;
        if (CreatePipe(&child_stdout_read, &child_stdout_write, &security, 0) == FALSE
            || CreatePipe(&child_stderr_read, &child_stderr_write, &security, 0) == FALSE
            || CreatePipe(&child_stdin_read, &child_stdin_write, &security, 0) == FALSE) {
            close_handle(child_stdout_read);
            close_handle(child_stdout_write);
            close_handle(child_stderr_read);
            close_handle(child_stderr_write);
            close_handle(child_stdin_read);
            close_handle(child_stdin_write);
            assign_error("failed to create agent process pipes", error);
            return false;
        }
        SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(child_stderr_read, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);

        ResolvedWindowsLaunch launch;
        if (!resolve_windows_launch(executable, arguments, &launch, error)) {
            close_handle(child_stdout_read);
            close_handle(child_stdout_write);
            close_handle(child_stderr_read);
            close_handle(child_stderr_write);
            close_handle(child_stdin_read);
            close_handle(child_stdin_write);
            return false;
        }
        std::vector<wchar_t> mutable_command(
            launch.command_line.begin(), launch.command_line.end());
        mutable_command.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = child_stdin_read;
        startup.hStdOutput = child_stdout_write;
        startup.hStdError = child_stderr_write;
        PROCESS_INFORMATION process{};
        const std::wstring cwd = working_directory.wstring();
        const BOOL created = CreateProcessW(
            launch.application_path.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, nullptr,
            cwd.empty() ? nullptr : cwd.c_str(), &startup, &process);
        close_handle(child_stdout_write);
        close_handle(child_stderr_write);
        close_handle(child_stdin_read);
        if (created == FALSE) {
            close_handle(child_stdout_read);
            close_handle(child_stderr_read);
            close_handle(child_stdin_write);
            assign_error("failed to start agent process", error);
            return false;
        }

        const bool contained = job_.attach_and_resume(process);
        if (!contained) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            close_handle(child_stdout_read);
            close_handle(child_stderr_read);
            close_handle(child_stdin_write);
            assign_error("cannot isolate agent process tree", error);
            return false;
        }
        process_handle_ = process.hProcess;
        process_id_ = process.dwProcessId;
        CloseHandle(process.hThread);
        stdin_write_ = child_stdin_write;
        running_.store(true);
        stdout_thread_ = std::thread(
            [this, handle = child_stdout_read, callback = std::move(stdout_line)]() mutable {
                read_lines(handle, std::move(callback));
            });
        stderr_thread_ = std::thread(
            [this, handle = child_stderr_read, callback = std::move(stderr_line)]() mutable {
                read_lines(handle, std::move(callback));
            });
        wait_thread_ = std::thread([this, callback = std::move(exited)]() mutable {
            WaitForSingleObject(process_handle_, INFINITE);
            DWORD exit_code = 1;
            GetExitCodeProcess(process_handle_, &exit_code);
            running_.store(false);
            if (callback) {
                callback(static_cast<int>(exit_code));
            }
        });
        return true;
    }

    bool write_line(const std::string& line, std::string* error) override {
        std::lock_guard lock(write_mutex_);
        if (!running_.load() || stdin_write_ == nullptr) {
            assign_error("agent process is not running", error);
            return false;
        }
        const std::string framed = line + "\n";
        HANDLE writer = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                &writer, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            assign_error("cannot establish cancellable Provider write", error);
            return false;
        }
        HANDLE deadline = nullptr;
        if (!CreateTimerQueueTimer(&deadline, nullptr,
                [](PVOID context, BOOLEAN) { CancelSynchronousIo(static_cast<HANDLE>(context)); },
                writer, 5000, 0, WT_EXECUTEONLYONCE)) {
            CloseHandle(writer);
            assign_error("cannot establish Provider write deadline", error);
            return false;
        }
        {
            std::lock_guard active_lock(active_write_mutex_);
            active_writer_ = writer;
        }
        DWORD written = 0;
        const BOOL result = WriteFile(stdin_write_, framed.data(), static_cast<DWORD>(framed.size()),
            &written, nullptr);
        const DWORD write_error = result ? ERROR_SUCCESS : GetLastError();
        DeleteTimerQueueTimer(nullptr, deadline, INVALID_HANDLE_VALUE);
        {
            std::lock_guard active_lock(active_write_mutex_);
            active_writer_ = nullptr;
            CloseHandle(writer);
        }
        if (!result || written != framed.size()) {
            assign_error(write_error == ERROR_OPERATION_ABORTED
                ? "Provider stdin write cancelled or exceeded 5 seconds"
                : "failed to write agent process stdin", error);
            return false;
        }
        return true;
    }

    void interrupt() override {
        if (running_.load() && process_id_ != 0) {
            GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process_id_);
        }
    }

    void stop() override {
        {
            std::lock_guard active_lock(active_write_mutex_);
            if (active_writer_) CancelSynchronousIo(active_writer_);
        }
        job_.terminate(130);
        running_.store(false);
        {
            std::lock_guard write_lock(write_mutex_);
            close_handle(stdin_write_);
        }
        if (stdout_thread_.joinable()) CancelSynchronousIo(stdout_thread_.native_handle());
        if (stderr_thread_.joinable()) CancelSynchronousIo(stderr_thread_.native_handle());
        join_thread(stdout_thread_);
        join_thread(stderr_thread_);
        join_thread(wait_thread_);
        close_handle(process_handle_);
        job_.reset();
        process_id_ = 0;
        running_.store(false);
    }

private:
    static void close_handle(HANDLE& handle) {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
        handle = nullptr;
    }

    static void join_thread(std::thread& thread) {
        if (!thread.joinable()) {
            return;
        }
        if (thread.get_id() == std::this_thread::get_id()) {
            thread.detach();
        } else {
            thread.join();
        }
    }

    void read_lines(HANDLE handle, AgentProcessLineCallback callback) {
        std::string buffered;
        std::array<char, 4096> chunk{};
        for (;;) {
            DWORD read = 0;
            if (ReadFile(handle, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr) == FALSE
                || read == 0) {
                break;
            }
            buffered.append(chunk.data(), read);
            for (;;) {
                const std::size_t newline = buffered.find('\n');
                if (newline == std::string::npos) {
                    break;
                }
                if (newline > 256U * 1024U) {
                    // A truncated JSON record could hide an approval or result.
                    // Fail only this owned Provider; its exit callback reports 125.
                    job_.terminate(125);
                    close_handle(handle);
                    return;
                }
                std::string line = buffered.substr(0, newline);
                buffered.erase(0, newline + 1U);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (callback) {
                    callback(std::move(line));
                }
            }
            if (buffered.size() > 256U * 1024U) {
                job_.terminate(125);
                close_handle(handle);
                return;
            }
        }
        if (!buffered.empty() && callback) {
            callback(std::move(buffered));
        }
        close_handle(handle);
    }

    std::atomic<bool> running_{false};
    OwnedAgentJob job_;
    HANDLE process_handle_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    DWORD process_id_ = 0;
    std::mutex write_mutex_;
    std::mutex active_write_mutex_;
    HANDLE active_writer_ = nullptr;
    std::thread stdout_thread_;
    std::thread stderr_thread_;
    std::thread wait_thread_;
};

#else

class UnsupportedAgentProcess final : public IAgentProcess {
public:
    bool executable_available(const std::string&, std::string*) override { return false; }
    bool run_probe(const std::string&, const std::vector<std::string>&, std::uint32_t,
                   int*, std::string*, std::string* error) override {
        assign_error("agent process is unavailable", error);
        return false;
    }
    bool start(const std::string&, const std::vector<std::string>&,
               const std::filesystem::path&, AgentProcessLineCallback,
               AgentProcessLineCallback, AgentProcessExitCallback,
               std::string* error) override {
        assign_error("agent processes are currently supported on Windows only", error);
        return false;
    }
    bool write_line(const std::string&, std::string* error) override {
        assign_error("agent process is unavailable", error);
        return false;
    }
    void interrupt() override {}
    void stop() override {}
};

#endif

}  // namespace

std::unique_ptr<IAgentProcess> make_system_agent_process() {
#ifdef _WIN32
    return std::make_unique<WindowsAgentProcess>();
#else
    return std::make_unique<UnsupportedAgentProcess>();
#endif
}

bool resolve_agent_command(
    const std::string& executable,
    const std::vector<std::string>& arguments,
    AgentResolvedCommand* resolved,
    std::string* error) {
    if (resolved == nullptr) {
        assign_error("resolved agent command output must be non-null", error);
        return false;
    }
#ifdef _WIN32
    ResolvedWindowsLaunch launch;
    if (!resolve_windows_launch(executable, arguments, &launch, error)) {
        return false;
    }
    std::vector<std::string> resolved_arguments;
    if (launch.kind == WindowsLaunchKind::kPowerShellScript) {
        resolved_arguments = {
            "-NoLogo", "-NoProfile", "-NonInteractive",
            "-ExecutionPolicy", "Bypass", "-File", wide_to_utf8(launch.target_path),
        };
        resolved_arguments.insert(
            resolved_arguments.end(), arguments.begin(), arguments.end());
    } else {
        resolved_arguments = arguments;
    }
    *resolved = {
        .application = std::filesystem::path(launch.application_path),
        .arguments = std::move(resolved_arguments),
        .target = std::filesystem::path(launch.target_path),
    };
    if (error != nullptr) {
        error->clear();
    }
    return true;
#else
    (void)executable;
    (void)arguments;
    assign_error("agent commands are currently supported on Windows only", error);
    return false;
#endif
}

}  // namespace redclaw::agent
