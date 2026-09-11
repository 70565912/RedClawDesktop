#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "redclaw/service/service_module.h"

namespace {

struct ServiceOptions {
    std::string listen_host = "127.0.0.1";
    std::uint16_t listen_port = 8467;
    std::string base_path = "/api";
    std::uint32_t run_seconds = 0;
    bool help_requested = false;
};

void print_usage() {
    std::cout
        << "Usage: redclaw_rendezvous_service [options]\n"
        << "Options:\n"
        << "  --listen-host <value>     Host or IP to bind (default 127.0.0.1)\n"
        << "  --listen-port <value>     TCP port to bind (default 8467)\n"
        << "  --base-path <value>       URL base path prefix (default /api)\n"
        << "  --run-seconds <value>     Exit after N seconds; 0 means long-running\n"
        << "  --help                    Show this help\n";
}

bool parse_uint16(std::string_view value, std::uint16_t* out) {
    if (value.empty()) {
        return false;
    }

    std::uint32_t parsed = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        parsed = (parsed * 10U) + static_cast<std::uint32_t>(ch - '0');
        if (parsed > 65535U) {
            return false;
        }
    }

    *out = static_cast<std::uint16_t>(parsed);
    return true;
}

bool parse_uint32(std::string_view value, std::uint32_t* out) {
    if (value.empty()) {
        return false;
    }

    std::uint64_t parsed = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        parsed = (parsed * 10ULL) + static_cast<std::uint64_t>(ch - '0');
        if (parsed > static_cast<std::uint64_t>(UINT32_MAX)) {
            return false;
        }
    }

    *out = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parse_uint64(std::string_view value, std::uint64_t* out) {
    if (value.empty()) {
        return false;
    }

    std::uint64_t parsed = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }

        const std::uint64_t next = (parsed * 10ULL) + static_cast<std::uint64_t>(ch - '0');
        if (next < parsed) {
            return false;
        }
        parsed = next;
    }

    *out = parsed;
    return true;
}

std::uint64_t now_unix_seconds() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

std::string normalize_session_code_copy(std::string_view value) {
    std::string normalized(value);
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
    return normalized;
}

std::string normalize_base_path(std::string value) {
    if (value.empty() || value == "/") {
        return "/";
    }

    if (value.front() != '/') {
        value.insert(value.begin(), '/');
    }

    while (value.size() > 1 && value.back() == '/') {
        value.pop_back();
    }

    return value;
}

bool parse_options(int argc, char** argv, ServiceOptions* options, std::string* error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            options->help_requested = true;
            return true;
        }

        if (arg == "--listen-host") {
            if (i + 1 >= argc) {
                *error = "missing value for --listen-host";
                return false;
            }
            options->listen_host = argv[++i];
            continue;
        }

        if (arg == "--listen-port") {
            if (i + 1 >= argc) {
                *error = "missing value for --listen-port";
                return false;
            }
            if (!parse_uint16(argv[++i], &options->listen_port)) {
                *error = "invalid --listen-port value";
                return false;
            }
            continue;
        }

        if (arg == "--base-path") {
            if (i + 1 >= argc) {
                *error = "missing value for --base-path";
                return false;
            }
            options->base_path = normalize_base_path(argv[++i]);
            continue;
        }

        if (arg == "--run-seconds") {
            if (i + 1 >= argc) {
                *error = "missing value for --run-seconds";
                return false;
            }
            if (!parse_uint32(argv[++i], &options->run_seconds)) {
                *error = "invalid --run-seconds value";
                return false;
            }
            continue;
        }

        *error = "unknown argument: " + arg;
        return false;
    }

    options->base_path = normalize_base_path(options->base_path);
    return true;
}

std::string escape_transport_field(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        if (ch == '\\') {
            escaped += "\\\\";
        } else if (ch == '\t') {
            escaped += "\\t";
        } else if (ch == '\n') {
            escaped += "\\n";
        } else if (ch == '\r') {
            escaped += "\\r";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

bool unescape_transport_field(std::string_view value, std::string* output) {
    output->clear();
    output->reserve(value.size());

    bool escape_pending = false;
    for (const char ch : value) {
        if (!escape_pending) {
            if (ch == '\\') {
                escape_pending = true;
            } else {
                output->push_back(ch);
            }
            continue;
        }

        if (ch == 'n') {
            output->push_back('\n');
        } else if (ch == 'r') {
            output->push_back('\r');
        } else if (ch == 't') {
            output->push_back('\t');
        } else if (ch == '\\') {
            output->push_back('\\');
        } else {
            return false;
        }
        escape_pending = false;
    }

    return !escape_pending;
}

bool parse_transport_document(
    std::string_view document,
    std::vector<std::vector<std::string>>* rows,
    std::string* error) {
    rows->clear();

    std::istringstream stream{std::string(document)};
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        std::vector<std::string> fields;
        std::string current;
        bool escape_pending = false;
        for (const char ch : line) {
            if (!escape_pending) {
                if (ch == '\\') {
                    escape_pending = true;
                    continue;
                }
                if (ch == '\t') {
                    std::string unescaped;
                    if (!unescape_transport_field(current, &unescaped)) {
                        *error = "failed to parse transport document field";
                        rows->clear();
                        return false;
                    }
                    fields.push_back(std::move(unescaped));
                    current.clear();
                    continue;
                }
                current.push_back(ch);
                continue;
            }

            current.push_back('\\');
            current.push_back(ch);
            escape_pending = false;
        }

        if (escape_pending) {
            *error = "transport document line ended with dangling escape";
            rows->clear();
            return false;
        }

        std::string unescaped;
        if (!unescape_transport_field(current, &unescaped)) {
            *error = "failed to parse transport document field";
            rows->clear();
            return false;
        }
        fields.push_back(std::move(unescaped));
        rows->push_back(std::move(fields));
    }

    return true;
}

std::string format_signal_event_line(const std::vector<std::string>& fields) {
    std::ostringstream out;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            out << '\t';
        }
        out << escape_transport_field(fields[i]);
    }
    out << '\n';
    return out.str();
}

std::string trim_copy(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(start, end - start));
}

std::string to_lower_ascii_copy(std::string_view value) {
    std::string lowered(value);
    std::transform(
        lowered.begin(),
        lowered.end(),
        lowered.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return lowered;
}

std::vector<std::string> split_path_segments(std::string_view path) {
    std::vector<std::string> segments;
    std::size_t index = 0;
    while (index < path.size()) {
        while (index < path.size() && path[index] == '/') {
            ++index;
        }
        const std::size_t begin = index;
        while (index < path.size() && path[index] != '/') {
            ++index;
        }
        if (begin < index) {
            segments.emplace_back(path.substr(begin, index - begin));
        }
    }
    return segments;
}

std::string map_registry_error(redclaw::service::RendezvousRegistryError error) {
    switch (error) {
    case redclaw::service::RendezvousRegistryError::kNone:
        return "none";
    case redclaw::service::RendezvousRegistryError::kInvalidRequest:
        return "invalid_request";
    case redclaw::service::RendezvousRegistryError::kCodeAlreadyExists:
        return "code_already_exists";
    case redclaw::service::RendezvousRegistryError::kCodeNotFound:
        return "not_found";
    case redclaw::service::RendezvousRegistryError::kCodeExpired:
        return "code_expired";
    case redclaw::service::RendezvousRegistryError::kCodeAlreadyClaimed:
        return "already_claimed";
    }

    return "unknown";
}

struct HttpRequest {
    std::string method;
    std::string target;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status_code = 200;
    std::string body;
};

struct StoredSignalSnapshot {
    std::string description_type;
    std::string description_sdp;
    std::vector<std::string> candidate_lines;
    std::uint64_t revision = 0;
    std::uint64_t updated_at_unix = 0;
};

class RendezvousServerState {
public:
    [[nodiscard]] HttpResponse register_session(std::string_view session_code, std::string_view request_body) {
        std::vector<std::vector<std::string>> rows;
        std::string parse_error;
        if (!parse_transport_document(request_body, &rows, &parse_error)
            || rows.empty()
            || rows[0].size() < 5
            || rows[0][0] != "register") {
            return {400, "invalid register body\n"};
        }

        redclaw::service::RendezvousSessionRegistration registration;
        registration.session_code = normalize_session_code_copy(session_code);
        registration.session_id = rows[0][1];
        registration.host_display_name = rows[0][2];
        registration.host_fingerprint_summary = rows[0][3];
        if (!parse_uint64(rows[0][4], &registration.ttl_seconds)) {
            return {400, "invalid register ttl\n"};
        }

        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_expired_locked();

        const auto result = registry_.register_session(registration);
        if (result.accepted) {
            session_expiry_by_id_[registration.session_id] = result.expires_at_unix;
            std::cout << "Rendezvous register accepted session_code=" << registration.session_code
                      << " session_id=" << registration.session_id
                      << " expires_at_unix=" << result.expires_at_unix << '\n';
        } else {
            std::cout << "Rendezvous register rejected session_code=" << registration.session_code
                      << " error=" << map_registry_error(result.error) << '\n';
        }

        return {
            200,
            format_signal_event_line({
                "result",
                result.accepted ? "1" : "0",
                map_registry_error(result.error),
                std::to_string(result.expires_at_unix),
            })};
    }

    [[nodiscard]] HttpResponse lookup_session(std::string_view session_code) {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_expired_locked();

        const auto result = registry_.lookup_session(normalize_session_code_copy(session_code));
        if (!result.found && result.error != redclaw::service::RendezvousRegistryError::kInvalidRequest) {
            return {404, {}};
        }

        return {
            200,
            format_signal_event_line({
                "lookup",
                result.found ? "1" : "0",
                result.claimed ? "1" : "0",
                map_registry_error(result.error),
                result.session_id,
                result.host_display_name,
                result.host_fingerprint_summary,
                std::to_string(result.expires_at_unix),
            })};
    }

    [[nodiscard]] HttpResponse claim_session(std::string_view session_code) {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_expired_locked();

        const auto result = registry_.claim_session(normalize_session_code_copy(session_code));
        if (!result.claimed
            && result.error != redclaw::service::RendezvousRegistryError::kInvalidRequest
            && result.error != redclaw::service::RendezvousRegistryError::kCodeAlreadyClaimed) {
            return {404, {}};
        }

        return {
            200,
            format_signal_event_line({
                "claim",
                result.claimed ? "1" : "0",
                map_registry_error(result.error),
                result.session_id,
            })};
    }

    [[nodiscard]] HttpResponse publish_signal(
        std::string_view session_id,
        std::string_view lane,
        std::string_view request_body) {
        std::vector<std::vector<std::string>> rows;
        std::string parse_error;
        if (!parse_transport_document(request_body, &rows, &parse_error)
            || rows.empty()
            || rows[0].size() < 3
            || rows[0][0] != "signal") {
            return {400, "invalid signal body\n"};
        }

        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_expired_locked();

        const std::string session_key(session_id);
        const auto session_it = session_expiry_by_id_.find(session_key);
        if (session_it == session_expiry_by_id_.end()) {
            return {404, {}};
        }

        StoredSignalSnapshot& snapshot = signals_[make_signal_key(session_id, lane)];
        snapshot.description_type = rows[0][1];
        snapshot.description_sdp = rows[0][2];
        snapshot.candidate_lines.clear();
        for (std::size_t i = 1; i < rows.size(); ++i) {
            if (rows[i].size() < 3 || rows[i][0] != "candidate") {
                continue;
            }
            snapshot.candidate_lines.push_back(rows[i][1] + "\t" + rows[i][2]);
        }
        ++snapshot.revision;
        snapshot.updated_at_unix = now_unix_seconds();

        std::cout << "Rendezvous publish accepted session_id=" << session_id
                  << " lane=" << lane
                  << " revision=" << snapshot.revision
                  << " candidates=" << snapshot.candidate_lines.size() << '\n';

        return {
            200,
            format_signal_event_line({
                "publish",
                "1",
                "none",
                std::to_string(snapshot.revision),
                std::to_string(snapshot.updated_at_unix),
            })};
    }

    [[nodiscard]] HttpResponse fetch_signal(std::string_view session_id, std::string_view lane) {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_expired_locked();

        const std::string session_key(session_id);
        if (!session_expiry_by_id_.contains(session_key)) {
            return {404, {}};
        }

        const auto it = signals_.find(make_signal_key(session_id, lane));
        if (it == signals_.end()) {
            return {404, {}};
        }

        std::string body = format_signal_event_line({
            "fetch",
            "1",
            "none",
            it->second.description_type,
            it->second.description_sdp,
            std::to_string(it->second.revision),
            std::to_string(it->second.updated_at_unix),
        });

        for (const std::string& candidate_line : it->second.candidate_lines) {
            const std::size_t separator = candidate_line.find('\t');
            if (separator == std::string::npos) {
                continue;
            }

            body.append(format_signal_event_line({
                "candidate",
                candidate_line.substr(0, separator),
                candidate_line.substr(separator + 1),
            }));
        }

        return {200, std::move(body)};
    }

private:
    [[nodiscard]] static std::string make_signal_key(std::string_view session_id, std::string_view lane) {
        return std::string(session_id) + "|" + std::string(lane);
    }

    void cleanup_expired_locked() {
        registry_.cleanup_expired();

        const std::uint64_t now = now_unix_seconds();
        for (auto it = session_expiry_by_id_.begin(); it != session_expiry_by_id_.end();) {
            if (now >= it->second) {
                const std::string session_id = it->first;
                signals_.erase(make_signal_key(session_id, "host"));
                signals_.erase(make_signal_key(session_id, "controller"));
                it = session_expiry_by_id_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::mutex mutex_;
    redclaw::service::InMemoryRendezvousSessionRegistry registry_;
    std::unordered_map<std::string, std::uint64_t> session_expiry_by_id_;
    std::unordered_map<std::string, StoredSignalSnapshot> signals_;
};

#ifdef _WIN32

struct ScopedWinsock {
    bool initialized = false;

    ~ScopedWinsock() {
        if (initialized) {
            WSACleanup();
        }
    }
};

struct ScopedSocket {
    SOCKET value = INVALID_SOCKET;

    ~ScopedSocket() {
        reset();
    }

    void reset(SOCKET next = INVALID_SOCKET) {
        if (value != INVALID_SOCKET) {
            closesocket(value);
        }
        value = next;
    }

    [[nodiscard]] bool valid() const {
        return value != INVALID_SOCKET;
    }
};

bool initialize_winsock(ScopedWinsock* winsock, std::string* error) {
    if (winsock->initialized) {
        return true;
    }

    WSADATA wsadata{};
    const int startup_result = WSAStartup(MAKEWORD(2, 2), &wsadata);
    if (startup_result != 0) {
        *error = "WSAStartup failed: " + std::to_string(startup_result);
        return false;
    }

    winsock->initialized = true;
    return true;
}

bool set_socket_non_blocking(SOCKET socket, std::string* error) {
    u_long non_blocking = 1;
    if (ioctlsocket(socket, FIONBIO, &non_blocking) != 0) {
        *error = "failed to set non-blocking mode, WSA error=" + std::to_string(WSAGetLastError());
        return false;
    }
    return true;
}

bool set_socket_blocking(SOCKET socket, std::string* error) {
    u_long non_blocking = 0;
    if (ioctlsocket(socket, FIONBIO, &non_blocking) != 0) {
        *error = "failed to set blocking mode, WSA error=" + std::to_string(WSAGetLastError());
        return false;
    }
    return true;
}

bool set_socket_timeouts(SOCKET socket, std::string* error) {
    constexpr DWORD kSocketTimeoutMs = 5000;
    if (setsockopt(
            socket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&kSocketTimeoutMs),
            sizeof(kSocketTimeoutMs)) != 0) {
        *error = "failed to set receive timeout, WSA error=" + std::to_string(WSAGetLastError());
        return false;
    }

    if (setsockopt(
            socket,
            SOL_SOCKET,
            SO_SNDTIMEO,
            reinterpret_cast<const char*>(&kSocketTimeoutMs),
            sizeof(kSocketTimeoutMs)) != 0) {
        *error = "failed to set send timeout, WSA error=" + std::to_string(WSAGetLastError());
        return false;
    }

    return true;
}

bool create_tcp_listener(const ServiceOptions& options, ScopedSocket* listener, std::string* error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* resolved = nullptr;
    const std::string service = std::to_string(options.listen_port);
    const char* host = (options.listen_host.empty() || options.listen_host == "0.0.0.0")
        ? nullptr
        : options.listen_host.c_str();
    const int gai_result = getaddrinfo(host, service.c_str(), &hints, &resolved);
    if (gai_result != 0) {
        *error = "failed to resolve listen address, error=" + std::to_string(gai_result);
        return false;
    }

    bool listening = false;
    for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
        SOCKET socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (socket == INVALID_SOCKET) {
            continue;
        }

        constexpr BOOL kReuseAddress = TRUE;
        setsockopt(
            socket,
            SOL_SOCKET,
            SO_REUSEADDR,
            reinterpret_cast<const char*>(&kReuseAddress),
            sizeof(kReuseAddress));

        if (::bind(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) != 0) {
            closesocket(socket);
            continue;
        }

        if (::listen(socket, SOMAXCONN) != 0) {
            closesocket(socket);
            continue;
        }

        if (!set_socket_non_blocking(socket, error)) {
            closesocket(socket);
            freeaddrinfo(resolved);
            return false;
        }

        listener->reset(socket);
        listening = true;
        break;
    }

    freeaddrinfo(resolved);
    if (!listening) {
        *error = "failed to bind rendezvous listener socket";
        return false;
    }

    return true;
}

bool try_accept_client(SOCKET listener, ScopedSocket* client, bool* accepted, std::string* error) {
    *accepted = false;

    sockaddr_storage address{};
    int length = sizeof(address);
    SOCKET accepted_socket = ::accept(listener, reinterpret_cast<sockaddr*>(&address), &length);
    if (accepted_socket == INVALID_SOCKET) {
        const int wsa_error = WSAGetLastError();
        if (wsa_error == WSAEWOULDBLOCK) {
            return true;
        }
        *error = "failed to accept rendezvous client, WSA error=" + std::to_string(wsa_error);
        return false;
    }

    if (!set_socket_timeouts(accepted_socket, error)) {
        closesocket(accepted_socket);
        return false;
    }
    if (!set_socket_blocking(accepted_socket, error)) {
        closesocket(accepted_socket);
        return false;
    }

    client->reset(accepted_socket);
    *accepted = true;
    return true;
}

bool send_socket_all(SOCKET socket, std::string_view payload, std::string* error) {
    std::size_t total_sent = 0;
    while (total_sent < payload.size()) {
        const int sent = ::send(
            socket,
            payload.data() + total_sent,
            static_cast<int>(payload.size() - total_sent),
            0);
        if (sent == SOCKET_ERROR) {
            *error = "failed to send response, WSA error=" + std::to_string(WSAGetLastError());
            return false;
        }
        total_sent += static_cast<std::size_t>(sent);
    }
    return true;
}

bool parse_http_request_headers(
    std::string_view header_block,
    HttpRequest* request,
    std::size_t* content_length,
    std::string* error) {
    request->headers.clear();
    *content_length = 0;

    std::istringstream stream{std::string(header_block)};
    std::string line;
    if (!std::getline(stream, line)) {
        *error = "missing HTTP request line";
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream request_line(line);
    std::string version;
    if (!(request_line >> request->method >> request->target >> version)) {
        *error = "invalid HTTP request line";
        return false;
    }

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }

        const std::size_t separator = line.find(':');
        if (separator == std::string::npos) {
            *error = "malformed HTTP header";
            return false;
        }

        const std::string name = to_lower_ascii_copy(line.substr(0, separator));
        const std::string value = trim_copy(line.substr(separator + 1));
        request->headers[name] = value;
    }

    const auto length_it = request->headers.find("content-length");
    if (length_it != request->headers.end()) {
        std::uint64_t parsed_length = 0;
        if (!parse_uint64(length_it->second, &parsed_length)) {
            *error = "invalid Content-Length header";
            return false;
        }
        *content_length = static_cast<std::size_t>(parsed_length);
    }

    return true;
}

bool receive_http_request(SOCKET socket, HttpRequest* request, std::string* error) {
    request->body.clear();
    std::string buffer;
    buffer.reserve(8192);

    std::size_t header_end = std::string::npos;
    std::size_t content_length = 0;
    bool headers_parsed = false;

    std::array<char, 4096> chunk{};
    while (true) {
        const int received = ::recv(socket, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (received == 0) {
            *error = "peer disconnected before request was complete";
            return false;
        }
        if (received == SOCKET_ERROR) {
            *error = "failed to receive request, WSA error=" + std::to_string(WSAGetLastError());
            return false;
        }

        buffer.append(chunk.data(), static_cast<std::size_t>(received));
        if (buffer.size() > (1024U * 1024U)) {
            *error = "request exceeded maximum supported size";
            return false;
        }

        if (!headers_parsed) {
            header_end = buffer.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                continue;
            }

            if (!parse_http_request_headers(
                    std::string_view(buffer.data(), header_end + 2),
                    request,
                    &content_length,
                    error)) {
                return false;
            }
            headers_parsed = true;
        }

        if (headers_parsed && buffer.size() >= header_end + 4 + content_length) {
            request->body.assign(buffer.data() + header_end + 4, content_length);
            return true;
        }
    }
}

std::string http_status_reason(int status_code) {
    switch (status_code) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 500:
        return "Internal Server Error";
    default:
        return "Unknown";
    }
}

bool send_http_response(SOCKET socket, const HttpResponse& response, std::string* error) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status_code << ' ' << http_status_reason(response.status_code) << "\r\n"
        << "Content-Type: text/plain; charset=utf-8\r\n"
        << "Connection: close\r\n"
        << "Content-Length: " << response.body.size() << "\r\n\r\n"
        << response.body;
    return send_socket_all(socket, out.str(), error);
}

std::optional<std::string> normalize_relative_target(
    std::string_view request_target,
    std::string_view base_path) {
    const std::size_t query = request_target.find('?');
    const std::string_view path = request_target.substr(0, query);
    if (path.empty() || path.front() != '/') {
        return std::nullopt;
    }

    if (base_path == "/") {
        return std::string(path);
    }

    if (!path.starts_with(base_path)) {
        return std::nullopt;
    }

    std::string relative(path.substr(base_path.size()));
    if (relative.empty()) {
        return "/";
    }
    if (relative.front() != '/') {
        return std::nullopt;
    }

    return relative;
}

HttpResponse dispatch_request(
    const ServiceOptions& options,
    RendezvousServerState* state,
    const HttpRequest& request) {
    const auto relative_target = normalize_relative_target(request.target, options.base_path);
    if (!relative_target.has_value()) {
        return {404, {}};
    }

    const std::vector<std::string> segments = split_path_segments(relative_target.value());
    if (segments.size() == 3 && segments[0] == "v1" && segments[1] == "sessions") {
        if (request.method == "PUT") {
            return state->register_session(segments[2], request.body);
        }
        if (request.method == "GET") {
            return state->lookup_session(segments[2]);
        }
        return {405, "method not allowed\n"};
    }

    if (segments.size() == 4 && segments[0] == "v1" && segments[1] == "sessions" && segments[3] == "claim") {
        if (request.method == "POST") {
            return state->claim_session(segments[2]);
        }
        return {405, "method not allowed\n"};
    }

    if (segments.size() == 4 && segments[0] == "v1" && segments[1] == "runtime-sessions") {
        const std::string& lane_segment = segments[3];
        if (lane_segment != "host-signal" && lane_segment != "controller-signal") {
            return {404, {}};
        }
        const std::string_view lane = lane_segment == "host-signal" ? "host" : "controller";
        if (request.method == "PUT") {
            return state->publish_signal(segments[2], lane, request.body);
        }
        if (request.method == "GET") {
            return state->fetch_signal(segments[2], lane);
        }
        return {405, "method not allowed\n"};
    }

    return {404, {}};
}

int run_service(const ServiceOptions& options) {
    ScopedWinsock winsock;
    std::string error;
    if (!initialize_winsock(&winsock, &error)) {
        std::cerr << "Rendezvous service startup failed: " << error << '\n';
        return 1;
    }

    ScopedSocket listener;
    if (!create_tcp_listener(options, &listener, &error)) {
        std::cerr << "Rendezvous service listener setup failed: " << error << '\n';
        return 1;
    }

    RendezvousServerState state;
    std::cout << "RedClaw rendezvous service started"
              << " listen_host=" << options.listen_host
              << " listen_port=" << options.listen_port
              << " base_path=" << options.base_path << '\n';

    const auto start_time = std::chrono::steady_clock::now();
    while (true) {
        if (options.run_seconds > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time);
            if (elapsed.count() >= options.run_seconds) {
                break;
            }
        }

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(listener.value, &read_set);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 250000;
        const int selected = ::select(0, &read_set, nullptr, nullptr, &timeout);
        if (selected == SOCKET_ERROR) {
            std::cerr << "Rendezvous service select failed, WSA error=" << WSAGetLastError() << '\n';
            return 1;
        }
        if (selected == 0) {
            continue;
        }

        ScopedSocket client;
        bool accepted = false;
        if (!try_accept_client(listener.value, &client, &accepted, &error)) {
            std::cerr << "Rendezvous service accept failed: " << error << '\n';
            return 1;
        }
        if (!accepted) {
            continue;
        }

        HttpRequest request;
        HttpResponse response;
        if (!receive_http_request(client.value, &request, &error)) {
            std::cerr << "Rendezvous service request read failed: " << error << '\n';
            response = {400, "bad request\n"};
        } else {
            response = dispatch_request(options, &state, request);
        }

        if (!send_http_response(client.value, response, &error)) {
            std::cerr << "Rendezvous service response write failed: " << error << '\n';
            return 1;
        }
    }

    std::cout << "RedClaw rendezvous service stopped" << '\n';
    return 0;
}

#endif

}  // namespace

int main(int argc, char** argv) {
    ServiceOptions options;
    std::string error;
    if (!parse_options(argc, argv, &options, &error)) {
        std::cerr << error << '\n';
        print_usage();
        return 1;
    }

    if (options.help_requested) {
        print_usage();
        return 0;
    }

#ifdef _WIN32
    return run_service(options);
#else
    std::cerr << "redclaw_rendezvous_service is only supported on Windows" << '\n';
    return 1;
#endif
}