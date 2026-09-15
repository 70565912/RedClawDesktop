#include "redclaw/workspace/terminal_session.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace redclaw::workspace {
namespace {
bool valid_size(TerminalSize size) {
    return size.columns >= 2 && size.rows >= 2 && size.columns <= 32767 && size.rows <= 32767;
}
void fail(std::string* error, const char* stage, unsigned long code = 0) {
    if (error) *error = std::string("terminal_") + stage + ":" + std::to_string(code);
}
#ifdef _WIN32
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    HANDLE* receive() { reset(); return &value_; }
    void reset(HANDLE value = nullptr) {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

std::vector<wchar_t> powershell_environment() {
    const auto inherited = GetEnvironmentStringsW();
    if (!inherited) return {};
    std::vector<wchar_t> environment;
    // An intermediate native process inherits PowerShell 7's module paths.
    // Let Windows PowerShell construct its own paths, without mutating Host's
    // environment or losing the private maintenance context and other settings.
    for (auto entry = inherited; *entry; entry += wcslen(entry) + 1) {
        const auto length = wcslen(entry) + 1;
        if (_wcsnicmp(entry, L"PSModulePath=", 13) != 0)
            environment.insert(environment.end(), entry, entry + length);
    }
    FreeEnvironmentStringsW(inherited);
    if (environment.empty()) environment.push_back(L'\0');
    environment.push_back(L'\0');
    return environment;
}
#endif
}

struct TerminalSession::Impl {
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::string> input, output;
    std::size_t input_bytes = 0, output_bytes = 0;
    std::optional<TerminalSize> pending_size;
    std::atomic_bool stopping = true;
    std::atomic_bool reader_done = true;
#ifdef _WIN32
    using CreateConsole = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
    using ResizeConsole = HRESULT(WINAPI*)(HPCON, COORD);
    using CloseConsole = void(WINAPI*)(HPCON);
    HPCON console = nullptr;
    ResizeConsole resize_console = nullptr;
    CloseConsole close_console = nullptr;
    Handle input_pipe, output_pipe, process, job;
    std::thread reader, writer;

    void read_loop() {
        for (;;) {
            std::string chunk(kOutputChunkBytes, '\0');
            DWORD count = 0;
            if (!ReadFile(output_pipe.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &count, nullptr)
                || count == 0) break;
            // During final shutdown keep draining ConPTY while ClosePseudoConsole
            // waits. During normal operation backpressure preserves every VT byte.
            if (stopping) continue;
            chunk.resize(count);
            std::unique_lock lock(mutex);
            changed.wait(lock, [&] {
                return stopping || output_bytes + chunk.size() <= kMaxBufferedOutputBytes;
            });
            if (stopping) continue;
            output_bytes += chunk.size();
            output.push_back(std::move(chunk));
        }
        reader_done = true;
    }

    void write_loop() {
        for (;;) {
            std::string bytes;
            std::optional<TerminalSize> size;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stopping || !input.empty() || pending_size.has_value(); });
                if (stopping) return;
                size = std::exchange(pending_size, std::nullopt);
                if (!input.empty()) {
                    bytes = std::move(input.front());
                    input.pop_front();
                    input_bytes -= bytes.size();
                }
            }
            if (size) (void)resize_console(console,
                COORD{static_cast<SHORT>(size->columns), static_cast<SHORT>(size->rows)});
            std::size_t offset = 0;
            while (!stopping && offset < bytes.size()) {
                DWORD written = 0;
                if (!WriteFile(input_pipe.get(), bytes.data() + offset,
                    static_cast<DWORD>(bytes.size() - offset), &written, nullptr) || !written) return;
                offset += written;
            }
        }
    }
#endif
};

TerminalSession::TerminalSession() : impl_(std::make_unique<Impl>()) {}
TerminalSession::~TerminalSession() { stop(); }

bool TerminalSession::start(const std::filesystem::path& directory, TerminalSize size, std::string* error) {
    if (error) error->clear();
    if (running()) { fail(error, "already_running"); return false; }
    stop();
    if (!valid_size(size) || !directory.is_absolute()) { fail(error, "invalid_start"); return false; }
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        fail(error, "working_directory_unavailable",
            attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY); return false;
    }
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto create_console = reinterpret_cast<Impl::CreateConsole>(GetProcAddress(kernel, "CreatePseudoConsole"));
    impl_->resize_console = reinterpret_cast<Impl::ResizeConsole>(GetProcAddress(kernel, "ResizePseudoConsole"));
    impl_->close_console = reinterpret_cast<Impl::CloseConsole>(GetProcAddress(kernel, "ClosePseudoConsole"));
    if (!create_console || !impl_->resize_console || !impl_->close_console) {
        fail(error, "conpty_unavailable"); return false;
    }
    Handle console_input, console_output;
    if (!CreatePipe(console_input.receive(), impl_->input_pipe.receive(), nullptr, 0)
        || !CreatePipe(impl_->output_pipe.receive(), console_output.receive(), nullptr, 0)) {
        fail(error, "pipe_create", GetLastError()); stop(); return false;
    }
    const auto result = create_console(COORD{static_cast<SHORT>(size.columns), static_cast<SHORT>(size.rows)},
        console_input.get(), console_output.get(), 0, &impl_->console);
    if (FAILED(result)) { fail(error, "conpty_create", static_cast<unsigned long>(result)); stop(); return false; }

    // Start draining before any child creation or error cleanup. Closing ConPTY
    // can emit a final screen update even when CreateProcess has not succeeded.
    impl_->stopping = false;
    impl_->reader_done = false;
    try {
        impl_->reader = std::thread([this] { impl_->read_loop(); });
    } catch (const std::system_error& exception) {
        console_input.reset(); console_output.reset();
        impl_->output_pipe.reset();
        fail(error, "reader_create", static_cast<unsigned long>(exception.code().value()));
        stop(); return false;
    }
    const auto cleanup = [&] {
        console_input.reset(); console_output.reset();
        stop();
    };

    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attributes_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes_list, 1, 0, &bytes)) {
        fail(error, "attributes_create", GetLastError()); cleanup(); return false;
    }
    const bool attached = UpdateProcThreadAttribute(attributes_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
        impl_->console, sizeof(HPCON), nullptr, nullptr) != FALSE;
    if (!attached) {
        const auto code = GetLastError();
        DeleteProcThreadAttributeList(attributes_list);
        fail(error, "attributes_attach", code); cleanup(); return false;
    }
    impl_->job.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!impl_->job.get() || !SetInformationJobObject(impl_->job.get(), JobObjectExtendedLimitInformation,
        &limits, sizeof(limits))) {
        const auto code = GetLastError();
        DeleteProcThreadAttributeList(attributes_list);
        fail(error, "job_create", code); cleanup(); return false;
    }
    wchar_t system[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length >= MAX_PATH) {
        DeleteProcThreadAttributeList(attributes_list);
        fail(error, "system_directory", GetLastError()); cleanup(); return false;
    }
    const auto executable = std::filesystem::path(system) / L"WindowsPowerShell/v1.0/powershell.exe";
    std::wstring command = L"\"" + executable.wstring() + L"\" -NoLogo -NoProfile";
    std::vector<wchar_t> module(32768);
    const auto module_length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (module_length && module_length < module.size()) {
        const auto profile = std::filesystem::path(std::wstring(module.data(), module_length)).parent_path() / L"maintenance/terminal-profile.ps1";
        const auto profile_attributes = GetFileAttributesW(profile.c_str());
        if (profile_attributes != INVALID_FILE_ATTRIBUTES && !(profile_attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            command += L" -NoExit -ExecutionPolicy Bypass -File \"" + profile.wstring() + L"\"";
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    // Null standard handles force attachment to ConPTY. Without this flag,
    // Windows can duplicate the runtime's redirected log pipes into the shell.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.lpAttributeList = attributes_list;
    auto environment = powershell_environment();
    if (environment.empty()) {
        const auto code = GetLastError();
        DeleteProcThreadAttributeList(attributes_list);
        fail(error, "environment_create", code); cleanup(); return false;
    }
    PROCESS_INFORMATION child{};
    const bool created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        environment.data(), directory.c_str(), &startup.StartupInfo, &child) != FALSE;
    const auto create_error = GetLastError();
    DeleteProcThreadAttributeList(attributes_list);
    if (!created) { fail(error, "shell_create", create_error); cleanup(); return false; }
    impl_->process.reset(child.hProcess);
    Handle initial_thread(child.hThread);
    if (!AssignProcessToJobObject(impl_->job.get(), child.hProcess)) {
        const auto code = GetLastError();
        TerminateProcess(child.hProcess, 1);
        fail(error, "job_attach", code); cleanup(); return false;
    }
    console_input.reset();
    console_output.reset();
    try {
        impl_->writer = std::thread([this] { impl_->write_loop(); });
    } catch (const std::system_error& exception) {
        fail(error, "writer_create", static_cast<unsigned long>(exception.code().value()));
        stop(); return false;
    }
    if (ResumeThread(initial_thread.get()) == static_cast<DWORD>(-1)) {
        fail(error, "shell_resume", GetLastError()); stop(); return false;
    }
    return true;
#else
    (void)directory; (void)size;
    fail(error, "unsupported_platform"); return false;
#endif
}

bool TerminalSession::write(std::string_view bytes) {
    if (bytes.empty() || bytes.size() > kMaxBufferedInputBytes || !running()) return false;
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->input_bytes + bytes.size() > kMaxBufferedInputBytes) return false;
    impl_->input.emplace_back(bytes);
    impl_->input_bytes += bytes.size();
    impl_->changed.notify_all();
    return true;
}

bool TerminalSession::resize(TerminalSize size) {
    if (!valid_size(size) || !running()) return false;
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) return false;
    impl_->pending_size = size;
    impl_->changed.notify_all();
    return true;
}

void TerminalSession::discard_pending_input() {
    std::lock_guard lock(impl_->mutex);
    impl_->input.clear();
    impl_->input_bytes = 0;
}

std::optional<std::string> TerminalSession::take_output() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->output.empty()) return {};
    auto bytes = std::move(impl_->output.front());
    impl_->output.pop_front();
    impl_->output_bytes -= bytes.size();
    impl_->changed.notify_all();
    return bytes;
}

bool TerminalSession::running() const {
#ifdef _WIN32
    return !impl_->stopping && impl_->process.get() && WaitForSingleObject(impl_->process.get(), 0) == WAIT_TIMEOUT;
#else
    return false;
#endif
}

std::size_t TerminalSession::buffered_output_bytes() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->output_bytes;
}
bool TerminalSession::output_finished() const {
    return impl_->reader_done && !running() && buffered_output_bytes() == 0;
}

void TerminalSession::stop() {
    impl_->stopping = true;
    impl_->changed.notify_all();
#ifdef _WIN32
    if (impl_->job.get()) TerminateJobObject(impl_->job.get(), 0);
    if (impl_->writer.joinable()) {
        // Cancellation can race a worker entering WriteFile. Repeat only during
        // shutdown until the worker exits; normal terminal operation never polls.
        do {
            CancelSynchronousIo(impl_->writer.native_handle());
        } while (WaitForSingleObject(impl_->writer.native_handle(), 20) == WAIT_TIMEOUT);
        impl_->writer.join();
    }
    impl_->input_pipe.reset();
    if (impl_->console) {
        impl_->close_console(impl_->console);
        impl_->console = nullptr;
    }
    if (impl_->reader.joinable()) {
        CancelSynchronousIo(impl_->reader.native_handle());
        impl_->reader.join();
    }
    impl_->output_pipe.reset();
    impl_->process.reset();
    impl_->job.reset();
#endif
    std::lock_guard lock(impl_->mutex);
    impl_->input.clear(); impl_->output.clear(); impl_->pending_size.reset();
    impl_->input_bytes = impl_->output_bytes = 0;
    impl_->reader_done = true;
}

}  // namespace redclaw::workspace
