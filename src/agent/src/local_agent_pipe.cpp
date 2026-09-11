#include "redclaw/agent/local_agent_pipe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#endif

namespace redclaw::agent {
namespace {
constexpr std::size_t kMaxQueueCount = 128;
constexpr std::size_t kMaxQueueBytes = 4U * 1024U * 1024U;
void assign_error(std::string value, std::string* error) {
    if (error) *error = std::move(value);
}
}  // namespace

class LocalAgentPipe::Impl {
public:
    mutable std::mutex mutex;
    std::deque<std::string> outgoing;
    struct Received { redclaw::protocol::AgentMessageEnvelopeV1 message; std::size_t bytes; };
    std::deque<Received> incoming;
    std::size_t outgoing_bytes = 0;
    std::size_t incoming_bytes = 0;
    LocalAgentPipeStats snapshot;
    std::atomic<std::uint32_t> expected_pid{0};
    std::jthread worker;
#ifdef _WIN32
    HANDLE cancel = nullptr;
    HANDLE wake = nullptr;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool server = false;
    struct Operation {
        OVERLAPPED overlap{};
        Operation() { overlap.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
        ~Operation() { if (overlap.hEvent) CloseHandle(overlap.hEvent); }
        void reset() { ResetEvent(overlap.hEvent); }
    };

    bool peer_matches() const {
        ULONG actual = 0;
        const BOOL read_pid = server ? GetNamedPipeClientProcessId(pipe, &actual)
                                     : GetNamedPipeServerProcessId(pipe, &actual);
        return read_pid && expected_pid.load() != 0 && actual == expected_pid.load();
    }

    void fail(std::string message) {
        std::lock_guard lock(mutex);
        snapshot.connected = false;
        snapshot.error = std::move(message);
    }

    void run_connection(std::stop_token stop, const std::wstring& path) {
        Operation connection;
        if (server) {
            const BOOL connected = ConnectNamedPipe(pipe, &connection.overlap);
            const DWORD code = connected ? ERROR_SUCCESS : GetLastError();
            if (!connected && code != ERROR_PIPE_CONNECTED && code != ERROR_IO_PENDING) {
                fail("Agent IPC connect failed"); return;
            }
            if (code == ERROR_IO_PENDING) {
                HANDLE events[]{cancel, connection.overlap.hEvent};
                if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) {
                    CancelIoEx(pipe, &connection.overlap);
                    DWORD ignored = 0;
                    GetOverlappedResult(pipe, &connection.overlap, &ignored, TRUE);
                    return;
                }
            }
        } else {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                    nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                if (pipe != INVALID_HANDLE_VALUE) break;
                if (WaitForSingleObject(cancel, 20) == WAIT_OBJECT_0) return;
            }
            if (pipe == INVALID_HANDLE_VALUE) { fail("Agent IPC unavailable"); return; }
        }
        // GUI sets the expected Runtime PID immediately after QProcess starts.
        for (int attempt = 0; expected_pid.load() == 0 && attempt < 250; ++attempt) {
            if (WaitForSingleObject(cancel, 20) == WAIT_OBJECT_0) return;
        }
        if (!peer_matches()) { fail("Agent IPC peer PID mismatch"); return; }
        {
            std::lock_guard lock(mutex);
            snapshot.connected = true;
            ++snapshot.connection_total;
            snapshot.error.clear();
        }
        Operation read;
        Operation write;
        std::array<char, 8192> chunk{};
        std::string buffered;
        bool reading = false;
        bool writing = false;
        std::string sending;
        auto write_started = std::chrono::steady_clock::now();
        std::string failure = "Agent IPC disconnected; desktop connection is unchanged";
        while (!stop.stop_requested()) {
            bool receive_space = true;
            while (buffered.size() >= 4) {
                const auto* header = reinterpret_cast<const unsigned char*>(buffered.data());
                const std::uint32_t size = header[0] | (header[1] << 8U)
                    | (header[2] << 16U) | (static_cast<std::uint32_t>(header[3]) << 24U);
                if (size == 0 || size > redclaw::protocol::kMaxAgentMessageBytes) {
                    failure = "Agent IPC frame exceeds limit"; receive_space = false; break;
                }
                if (buffered.size() < size + 4U) break;
                std::lock_guard lock(mutex);
                if (incoming.size() >= kMaxQueueCount || incoming_bytes + size > kMaxQueueBytes) {
                    receive_space = false; break;
                }
                auto parsed = redclaw::protocol::parse_agent_message_v1(std::string_view(buffered).substr(4, size));
                if (!parsed.ok) {
                    failure = "Agent IPC invalid envelope"; receive_space = false; break;
                }
                const auto expanded = redclaw::protocol::agent_message_protobuf_size_v1(parsed.value);
                if (incoming_bytes + expanded > kMaxQueueBytes) {
                    receive_space = false; break;
                }
                incoming_bytes += expanded;
                incoming.push_back({std::move(parsed.value), expanded});
                ++snapshot.received_total;
                buffered.erase(0, size + 4U);
            }
            if (failure != "Agent IPC disconnected; desktop connection is unchanged") break;
            {
                std::lock_guard lock(mutex);
                receive_space = receive_space && incoming.size() < kMaxQueueCount
                    && incoming_bytes + redclaw::protocol::kMaxAgentMessageBytes <= kMaxQueueBytes;
                if (!writing && !outgoing.empty()) {
                    sending = std::move(outgoing.front());
                    outgoing.pop_front();
                    outgoing_bytes -= sending.size();
                }
            }
            // Backpressure pauses reads, not the connection. One independent
            // pending write must never prevent this worker from completing reads.
            if (!reading && receive_space) {
                read.reset();
                DWORD count = 0;
                const BOOL done = ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()),
                    &count, &read.overlap);
                if (!done && GetLastError() != ERROR_IO_PENDING) break;
                if (done) SetEvent(read.overlap.hEvent);
                reading = true;
            }
            if (!writing && !sending.empty()) {
                write.reset();
                DWORD count = 0;
                const BOOL done = WriteFile(pipe, sending.data(), static_cast<DWORD>(sending.size()), &count, &write.overlap);
                if (!done && GetLastError() != ERROR_IO_PENDING) break;
                if (done) SetEvent(write.overlap.hEvent);
                writing = true;
                write_started = std::chrono::steady_clock::now();
            }
            HANDLE events[]{cancel, read.overlap.hEvent, write.overlap.hEvent, wake};
            const DWORD event = WaitForMultipleObjects(4, events, FALSE, 50);
            if (event == WAIT_OBJECT_0) break;
            if (reading && WaitForSingleObject(read.overlap.hEvent, 0) == WAIT_OBJECT_0) {
                DWORD count = 0;
                if (!GetOverlappedResult(pipe, &read.overlap, &count, FALSE) || count == 0) break;
                reading = false;
                read.reset();
                buffered.append(chunk.data(), count);
            }
            if (writing && WaitForSingleObject(write.overlap.hEvent, 0) == WAIT_OBJECT_0) {
                DWORD count = 0;
                if (!GetOverlappedResult(pipe, &write.overlap, &count, FALSE) || count != sending.size()) break;
                writing = false;
                write.reset();
                sending.clear();
                std::lock_guard lock(mutex);
                ++snapshot.sent_total;
            }
            if (writing && std::chrono::steady_clock::now() - write_started > std::chrono::seconds(5)) {
                failure = "Agent IPC write deadline exceeded"; break;
            }
        }
        if (reading) {
            CancelIoEx(pipe, &read.overlap);
            DWORD ignored = 0;
            GetOverlappedResult(pipe, &read.overlap, &ignored, TRUE);
        }
        if (writing) {
            CancelIoEx(pipe, &write.overlap);
            DWORD ignored = 0;
            GetOverlappedResult(pipe, &write.overlap, &ignored, TRUE);
        }
        fail(std::move(failure));
    }

    void run(std::stop_token stop, const std::wstring& path) {
        while (!stop.stop_requested()) {
            try { run_connection(stop, path); }
            catch (const std::exception&) { fail("Agent IPC worker failed"); }
            if (pipe != INVALID_HANDLE_VALUE) {
                CancelIoEx(pipe, nullptr);
                if (server) DisconnectNamedPipe(pipe);
                else { CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; }
            }
            if (WaitForSingleObject(cancel, 100) == WAIT_OBJECT_0) break;
        }
    }
#endif
};

LocalAgentPipe::LocalAgentPipe() : impl_(std::make_unique<Impl>()) {}
LocalAgentPipe::~LocalAgentPipe() { stop(); }

bool LocalAgentPipe::start(std::string name, bool server, std::uint32_t pid, std::string* error) {
    stop();
#ifdef _WIN32
    if (name.empty() || name.size() > 128 || name.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos
        || (!server && pid == 0)) {
        assign_error("invalid Agent IPC launch identity", error); return false;
    }
    impl_ = std::make_unique<Impl>();
    impl_->expected_pid.store(pid);
    impl_->server = server;
    impl_->cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->cancel || !impl_->wake) { assign_error("Agent IPC event creation failed", error); stop(); return false; }
    const std::wstring path = L"\\\\.\\pipe\\" + std::wstring(name.begin(), name.end());
    if (server) {
        HANDLE token = nullptr;
        DWORD size = 0;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> token_info(size);
        const BOOL got_user = GetTokenInformation(token, TokenUser, token_info.data(), size, &size);
        CloseHandle(token);
        LPWSTR sid = nullptr;
        if (!got_user || !ConvertSidToStringSidW(
                reinterpret_cast<TOKEN_USER*>(token_info.data())->User.Sid, &sid)) return false;
        const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        impl_->pipe = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX
            | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 65536, 65536, 0, &security);
        LocalFree(descriptor);
        if (impl_->pipe == INVALID_HANDLE_VALUE) { assign_error("Agent IPC listen failed", error); stop(); return false; }
    }
    impl_->worker = std::jthread([this, path](std::stop_token stop) { impl_->run(stop, path); });
    return true;
#else
    (void)name; (void)server; (void)pid;
    assign_error("native Agent IPC requires Windows", error);
    return false;
#endif
}

void LocalAgentPipe::set_expected_peer_pid(std::uint32_t pid) { impl_->expected_pid.store(pid); }

void LocalAgentPipe::stop() {
#ifdef _WIN32
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        SetEvent(impl_->cancel);
        impl_->worker.join();
    }
    if (impl_->pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(impl_->pipe, nullptr);
        CloseHandle(impl_->pipe);
        impl_->pipe = INVALID_HANDLE_VALUE;
    }
    if (impl_->cancel) { CloseHandle(impl_->cancel); impl_->cancel = nullptr; }
    if (impl_->wake) { CloseHandle(impl_->wake); impl_->wake = nullptr; }
#endif
    std::lock_guard lock(impl_->mutex);
    impl_->snapshot.connected = false;
}

bool LocalAgentPipe::send(const redclaw::protocol::AgentMessageEnvelopeV1& message, std::string* error) {
    if (!redclaw::protocol::validate_agent_message_v1(message, error)) return false;
    auto payload = redclaw::protocol::serialize_agent_message_v1(message);
    if (payload.empty() || payload.size() > redclaw::protocol::kMaxAgentMessageBytes) return false;
    std::string bytes(4, '\0');
    for (unsigned int i = 0; i < 4; ++i) bytes[i] = static_cast<char>((payload.size() >> (8U * i)) & 255U);
    bytes += payload;
    std::lock_guard lock(impl_->mutex);
    if (!impl_->snapshot.connected || impl_->outgoing.size() >= kMaxQueueCount
        || impl_->outgoing_bytes + bytes.size() > kMaxQueueBytes) {
        ++impl_->snapshot.rejected_total;
        assign_error("agent_busy: local IPC unavailable or send queue full", error);
        return false;
    }
    impl_->outgoing_bytes += bytes.size();
    impl_->outgoing.push_back(std::move(bytes));
#ifdef _WIN32
    SetEvent(impl_->wake);
#endif
    return true;
}

std::vector<redclaw::protocol::AgentMessageEnvelopeV1> LocalAgentPipe::take_received(std::size_t limit) {
    std::vector<redclaw::protocol::AgentMessageEnvelopeV1> result;
    std::lock_guard lock(impl_->mutex);
    while (!impl_->incoming.empty() && result.size() < limit) {
        impl_->incoming_bytes -= impl_->incoming.front().bytes;
        result.push_back(std::move(impl_->incoming.front().message));
        impl_->incoming.pop_front();
    }
#ifdef _WIN32
    if (!result.empty()) SetEvent(impl_->wake);
#endif
    return result;
}

LocalAgentPipeStats LocalAgentPipe::stats() const {
    std::lock_guard lock(impl_->mutex);
    auto result = impl_->snapshot;
    result.received_depth = impl_->incoming.size();
    result.send_depth = impl_->outgoing.size();
    return result;
}
}  // namespace redclaw::agent
