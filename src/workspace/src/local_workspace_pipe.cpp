#include "redclaw/workspace/local_workspace_pipe.h"
#include "redclaw/protocol/compressed_protobuf.h"
#include <array>
#include <algorithm>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace redclaw::workspace {
namespace {
constexpr auto kMaximumFrame = protocol::kMaxProtobufWireBytes;
bool valid_name(std::string_view name) {
    if (!name.starts_with("RedClawDesktop.Workspace.v1.") || name.size() > 160) return false;
    for (const char ch : name) if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-')) return false;
    return true;
}
}
struct LocalWorkspacePipe::Impl {
    bool connected = false;
    std::string incoming, outgoing;
#ifdef _WIN32
    HANDLE pipe = INVALID_HANDLE_VALUE;
    OVERLAPPED read{}, write{};
    bool read_pending = false, write_pending = false;
    std::size_t written = 0;
    std::array<char, 16 * 1024> input{};
    bool advance_write() {
        if (outgoing.empty()) return true;
        DWORD count = 0;
        if (write_pending) {
            if (!GetOverlappedResult(pipe, &write, &count, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) return true;
                return false;
            }
            write_pending = false;
            if (!count) return false;
            written += count;
        }
        if (written == outgoing.size()) { outgoing.clear(); written = 0; return true; }
        ResetEvent(write.hEvent);
        if (!WriteFile(pipe, outgoing.data() + written, static_cast<DWORD>(outgoing.size() - written), &count, &write)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            write_pending = true;
        } else {
            if (!count) return false;
            written += count;
            if (written == outgoing.size()) { outgoing.clear(); written = 0; }
        }
        return true;
    }
#endif
};
LocalWorkspacePipe::LocalWorkspacePipe() : impl_(std::make_unique<Impl>()) {}
LocalWorkspacePipe::~LocalWorkspacePipe() { close(); }
bool LocalWorkspacePipe::connect(std::string_view name, std::uint32_t gui_pid, std::string* error) {
    close();
    const auto fail = [&](const char* stage) { if (error) *error = stage; close(); return false; };
    if (!valid_name(name) || !gui_pid) return fail("workspace_pipe_invalid_identity");
#ifdef _WIN32
    const std::wstring path = L"\\\\.\\pipe\\" + std::wstring(name.begin(), name.end());
    impl_->pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (impl_->pipe == INVALID_HANDLE_VALUE) return fail("workspace_pipe_connect_failed");
    ULONG server_pid = 0;
    if (!GetNamedPipeServerProcessId(impl_->pipe, &server_pid) || server_pid != gui_pid) return fail("workspace_pipe_owner_mismatch");
    impl_->read.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->write.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->read.hEvent || !impl_->write.hEvent) return fail("workspace_pipe_event_failed");
    impl_->connected = true;
    if (error) error->clear();
    return true;
#else
    return fail("workspace_pipe_unsupported");
#endif
}
bool LocalWorkspacePipe::send(std::string_view frame) {
    if (!writable() || frame.empty() || frame.size() > kMaximumFrame) return false;
#ifdef _WIN32
    impl_->outgoing.resize(4 + frame.size());
    for (unsigned index = 0; index < 4; ++index) impl_->outgoing[index] = static_cast<char>((frame.size() >> (8 * index)) & 255);
    impl_->outgoing.replace(4, frame.size(), frame);
    if (!impl_->advance_write()) { close(); return false; }
    return true;
#else
    return false;
#endif
}
void LocalWorkspacePipe::poll(const std::function<void(std::string_view)>& receive) {
#ifdef _WIN32
    if (!impl_->connected) return;
    if (!impl_->advance_write()) { close(); return; }
    // A bounded amount of workspace work per existing scheduler iteration.
    for (int iteration = 0; iteration < 4 && impl_->connected; ++iteration) {
        DWORD count = 0;
        if (impl_->read_pending) {
            if (!GetOverlappedResult(impl_->pipe, &impl_->read, &count, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) break;
                close(); return;
            }
            impl_->read_pending = false;
        } else {
            ResetEvent(impl_->read.hEvent);
            if (!ReadFile(impl_->pipe, impl_->input.data(), static_cast<DWORD>(impl_->input.size()), &count, &impl_->read)) {
                if (GetLastError() == ERROR_IO_PENDING) { impl_->read_pending = true; break; }
                close(); return;
            }
        }
        if (!count || impl_->incoming.size() + count > kMaximumFrame + 4 + impl_->input.size()) { close(); return; }
        impl_->incoming.append(impl_->input.data(), count);
        while (impl_->incoming.size() >= 4) {
            std::uint32_t length = 0;
            for (unsigned index = 0; index < 4; ++index) length |= static_cast<std::uint32_t>(static_cast<unsigned char>(impl_->incoming[index])) << (8 * index);
            if (!length || length > kMaximumFrame) { close(); return; }
            if (impl_->incoming.size() < 4 + length) break;
            // Callback must not reenter this pipe. Its view expires on return.
            receive(std::string_view(impl_->incoming).substr(4, length));
            impl_->incoming.erase(0, 4 + length);
        }
    }
#else
    (void)receive;
#endif
}
bool LocalWorkspacePipe::connected() const { return impl_->connected; }
bool LocalWorkspacePipe::writable() const { return impl_->connected && impl_->outgoing.empty(); }
void LocalWorkspacePipe::close() {
#ifdef _WIN32
    if (impl_->pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(impl_->pipe, nullptr);
        DWORD ignored = 0;
        if (impl_->read_pending) GetOverlappedResult(impl_->pipe, &impl_->read, &ignored, TRUE);
        if (impl_->write_pending) GetOverlappedResult(impl_->pipe, &impl_->write, &ignored, TRUE);
        CloseHandle(impl_->pipe); impl_->pipe = INVALID_HANDLE_VALUE;
    }
    if (impl_->read.hEvent) CloseHandle(impl_->read.hEvent);
    if (impl_->write.hEvent) CloseHandle(impl_->write.hEvent);
    impl_->read = {}; impl_->write = {};
    impl_->read_pending = impl_->write_pending = false;
    impl_->written = 0;
#endif
    impl_->incoming.clear(); impl_->outgoing.clear(); impl_->connected = false;
}
}
