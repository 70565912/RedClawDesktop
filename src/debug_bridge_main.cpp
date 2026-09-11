#include "redclaw/agent/agent_providers.h"
#include "redclaw/agent/remote_agent_broker.h"
#include "redclaw/net/net_module.h"
#include "redclaw/protocol/agent_protocol.h"
#include "redclaw/protocol/debug_bridge_protocol.h"
#include "redclaw/protocol/protocol_module.h"
#include "redclaw/protocol/sdp_signaling.h"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <sddl.h>
#endif

namespace {

using namespace std::chrono_literals;

constexpr std::size_t kMaxLocalControlBytes = 64U * 1024U;
constexpr std::size_t kMaxAgentEventRecords = 256U;
constexpr std::uint64_t kRouteCacheLifetimeMs = 30ULL * 24ULL * 60ULL * 60ULL * 1000ULL;

enum class BridgeRole {
    kHost,
    kController,
};

struct BridgeOptions {
    BridgeRole role = BridgeRole::kHost;
    std::filesystem::path signal_directory;
    std::filesystem::path passphrase_file;
    std::filesystem::path route_cache_file;
    std::filesystem::path agent_project_manifest;
    std::string bridge_id;
    std::string session_epoch;
    std::string bind_address;
    std::string network_fingerprint;
    std::string local_control_name = "RedClawDesktop.DebugBridge.v1";
    std::string redclaw_control_name = "RedClawDesktop.DebugControl.v1";
    std::vector<std::string> ice_servers;
    std::uint32_t run_seconds = 0;
    bool allow_agent_tasks = false;
};

struct RouteCacheRecord {
    std::string network_fingerprint;
    std::string bind_address;
    std::vector<std::string> ice_servers;
    std::uint16_t preferred_port = 0;
    std::uint64_t expires_at_ms = 0;
};

struct InboundItem {
    redclaw::protocol::DebugBridgeEnvelopeV1 envelope;
};

struct PendingResponse {
    std::mutex mutex;
    std::condition_variable cv;
    std::string response;
    bool complete = false;
};

struct LocalRequest {
    redclaw::protocol::DebugBridgeEnvelopeV1 envelope;
    std::shared_ptr<PendingResponse> pending;
    bool complete_after_send = false;
};

std::uint64_t now_unix_ms() {
    const auto now = std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

std::string role_name(BridgeRole role) {
    return role == BridgeRole::kHost ? "host" : "controller";
}

bool valid_token(std::string_view value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char ch) {
        const unsigned char value = static_cast<unsigned char>(ch);
        return std::isalnum(value) != 0 || ch == '-' || ch == '_' || ch == '.';
    });
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::string value(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    return value;
}

bool write_text_file_atomically(
    const std::filesystem::path& path,
    std::string_view contents,
    std::string* error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        *error = "failed to create signaling directory";
        return false;
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            *error = "failed to create temporary signaling file";
            return false;
        }
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output) {
            *error = "failed to write temporary signaling file";
            return false;
        }
    }
    std::filesystem::remove(path, filesystem_error);
    filesystem_error.clear();
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        *error = "failed to replace signaling file";
        return false;
    }
    error->clear();
    return true;
}

std::optional<std::uint16_t> candidate_port(std::string_view candidate_line) {
    const std::size_t tab = candidate_line.find('\t');
    std::string_view candidate = tab == std::string_view::npos
        ? candidate_line : candidate_line.substr(tab + 1U);
    std::vector<std::string_view> fields;
    std::size_t begin = 0;
    while (begin < candidate.size()) {
        while (begin < candidate.size() && candidate[begin] == ' ') {
            ++begin;
        }
        const std::size_t end = candidate.find(' ', begin);
        fields.push_back(candidate.substr(
            begin,
            end == std::string_view::npos ? candidate.size() - begin : end - begin));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    if (fields.size() <= 5U) {
        return std::nullopt;
    }
    unsigned int parsed = 0;
    const auto converted = std::from_chars(
        fields[5].data(), fields[5].data() + fields[5].size(), parsed);
    if (converted.ec != std::errc{} || parsed == 0 || parsed > 65535U) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(parsed);
}

#if defined(_WIN32)
bool protect_local_data(std::string_view plaintext, std::vector<std::uint8_t>* protected_data) {
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    input.cbData = static_cast<DWORD>(plaintext.size());
    DATA_BLOB output{};
    if (CryptProtectData(
            &input,
            L"RedClaw debug bridge route cache",
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &output) == FALSE) {
        return false;
    }
    protected_data->assign(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return true;
}

bool unprotect_local_data(
    const std::vector<std::uint8_t>& protected_data,
    std::string* plaintext) {
    DATA_BLOB input{};
    input.pbData = const_cast<BYTE*>(protected_data.data());
    input.cbData = static_cast<DWORD>(protected_data.size());
    DATA_BLOB output{};
    if (CryptUnprotectData(
            &input,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &output) == FALSE) {
        return false;
    }
    plaintext->assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return true;
}
#endif

std::optional<RouteCacheRecord> load_route_cache(
    const std::filesystem::path& path,
    std::string_view expected_network_fingerprint) {
#if defined(_WIN32)
    if (path.empty() || expected_network_fingerprint.empty()) {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> encrypted(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    std::string plaintext;
    if (encrypted.empty() || !unprotect_local_data(encrypted, &plaintext)) {
        return std::nullopt;
    }
    boost::system::error_code parse_error;
    const boost::json::value value = boost::json::parse(plaintext, parse_error);
    if (parse_error || !value.is_object()) {
        return std::nullopt;
    }
    const auto& object = value.as_object();
    const auto* fingerprint = object.if_contains("network_fingerprint");
    const auto* bind = object.if_contains("bind_address");
    const auto* expires = object.if_contains("expires_at_ms");
    const auto* port = object.if_contains("preferred_port");
    const auto* servers = object.if_contains("ice_servers");
    const auto unsigned_value = [](const boost::json::value* value)
        -> std::optional<std::uint64_t> {
        if (value == nullptr) {
            return std::nullopt;
        }
        if (value->is_uint64()) {
            return value->as_uint64();
        }
        if (value->is_int64() && value->as_int64() >= 0) {
            return static_cast<std::uint64_t>(value->as_int64());
        }
        return std::nullopt;
    };
    const auto expires_value = unsigned_value(expires);
    const auto port_value = unsigned_value(port);
    if (fingerprint == nullptr || !fingerprint->is_string()
        || fingerprint->as_string() != expected_network_fingerprint
        || bind == nullptr || !bind->is_string()
        || !expires_value.has_value() || *expires_value <= now_unix_ms()
        || !port_value.has_value() || *port_value > 65535U
        || servers == nullptr || !servers->is_array()) {
        return std::nullopt;
    }
    RouteCacheRecord record;
    record.network_fingerprint = std::string(fingerprint->as_string());
    record.bind_address = std::string(bind->as_string());
    record.expires_at_ms = *expires_value;
    record.preferred_port = static_cast<std::uint16_t>(*port_value);
    for (const auto& server : servers->as_array()) {
        if (!server.is_string()
            || !std::string_view(server.as_string().data(), server.as_string().size())
                    .starts_with("stun:")) {
            return std::nullopt;
        }
        record.ice_servers.emplace_back(server.as_string());
    }
    return record;
#else
    (void)path;
    (void)expected_network_fingerprint;
    return std::nullopt;
#endif
}

bool save_route_cache(const std::filesystem::path& path, const RouteCacheRecord& record) {
#if defined(_WIN32)
    if (path.empty() || record.network_fingerprint.empty()) {
        return false;
    }
    boost::json::array servers;
    for (const auto& server : record.ice_servers) {
        servers.emplace_back(server);
    }
    boost::json::object object;
    object["schema"] = "redclaw.debug-bridge-route-cache.v1";
    object["network_fingerprint"] = record.network_fingerprint;
    object["bind_address"] = record.bind_address;
    object["ice_servers"] = std::move(servers);
    object["preferred_port"] = record.preferred_port;
    object["expires_at_ms"] = record.expires_at_ms;
    std::vector<std::uint8_t> encrypted;
    if (!protect_local_data(boost::json::serialize(object), &encrypted)) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return false;
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(
            reinterpret_cast<const char*>(encrypted.data()),
            static_cast<std::streamsize>(encrypted.size()));
        if (!output) {
            return false;
        }
    }
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    return !error;
#else
    (void)path;
    (void)record;
    return false;
#endif
}

void print_usage() {
    std::cout
        << "RedClaw debug bridge\n"
        << "  --role host|controller --signal-dir <path> --passphrase-file <path>\n"
        << "  --bridge-id <token> --session-epoch <token> [--bind-address <ip>]\n"
        << "  [--ice-server <stun-uri>] [--network-fingerprint <token>]\n"
        << "  [--route-cache <path>] [--control-name <name>] [--redclaw-control-name <name>]\n"
        << "  [--allow-agent-tasks --agent-project-manifest <path>] [--run-seconds <n>]\n";
}

bool parse_options(int argc, char** argv, BridgeOptions* options, std::string* error) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto require_value = [&](std::string* target) -> bool {
            if (index + 1 >= argc) {
                *error = "missing value after " + argument;
                return false;
            }
            *target = argv[++index];
            return true;
        };
        std::string value;
        if (argument == "--help" || argument == "-h") {
            print_usage();
            return false;
        }
        if (argument == "--role") {
            if (!require_value(&value)) return false;
            if (value == "host") options->role = BridgeRole::kHost;
            else if (value == "controller") options->role = BridgeRole::kController;
            else {
                *error = "role must be host or controller";
                return false;
            }
        } else if (argument == "--signal-dir") {
            if (!require_value(&value)) return false;
            options->signal_directory = value;
        } else if (argument == "--passphrase-file") {
            if (!require_value(&value)) return false;
            options->passphrase_file = value;
        } else if (argument == "--bridge-id") {
            if (!require_value(&options->bridge_id)) return false;
        } else if (argument == "--session-epoch") {
            if (!require_value(&options->session_epoch)) return false;
        } else if (argument == "--bind-address") {
            if (!require_value(&options->bind_address)) return false;
        } else if (argument == "--network-fingerprint") {
            if (!require_value(&options->network_fingerprint)) return false;
        } else if (argument == "--route-cache") {
            if (!require_value(&value)) return false;
            options->route_cache_file = value;
        } else if (argument == "--control-name") {
            if (!require_value(&options->local_control_name)) return false;
        } else if (argument == "--redclaw-control-name") {
            if (!require_value(&options->redclaw_control_name)) return false;
        } else if (argument == "--ice-server") {
            if (!require_value(&value)) return false;
            if (!std::string_view(value).starts_with("stun:")) {
                *error = "the P2P debug bridge accepts STUN servers only";
                return false;
            }
            options->ice_servers.push_back(std::move(value));
        } else if (argument == "--agent-project-manifest") {
            if (!require_value(&value)) return false;
            options->agent_project_manifest = value;
        } else if (argument == "--allow-agent-tasks") {
            options->allow_agent_tasks = true;
        } else if (argument == "--run-seconds") {
            if (!require_value(&value)) return false;
            unsigned long parsed = 0;
            try {
                parsed = std::stoul(value);
            } catch (...) {
                *error = "run-seconds must be an integer";
                return false;
            }
            if (parsed > (std::numeric_limits<std::uint32_t>::max)()) {
                *error = "run-seconds exceeds supported range";
                return false;
            }
            options->run_seconds = static_cast<std::uint32_t>(parsed);
        } else {
            *error = "unknown argument: " + argument;
            return false;
        }
    }

    if (options->signal_directory.empty()
        || options->passphrase_file.empty()
        || !valid_token(options->bridge_id, 128)
        || !valid_token(options->session_epoch, 128)
        || options->local_control_name.empty()
        || options->local_control_name.size() > 128U
        || options->redclaw_control_name.empty()
        || options->redclaw_control_name.size() > 128U) {
        *error = "required bridge options are missing or invalid";
        return false;
    }
    if (options->allow_agent_tasks && options->role == BridgeRole::kHost
        && options->agent_project_manifest.empty()) {
        *error = "Host Agent authorization requires a registered-project manifest";
        return false;
    }
    if (options->ice_servers.empty()) {
        options->ice_servers = {
            "stun:stun.douyucdn.cn:18000",
            "stun:stun.l.google.com:19302",
            "stun:stun.cloudflare.com:3478",
        };
    }
    return true;
}

boost::json::object make_local_error(
    std::string_view request_id,
    std::string_view code,
    std::string_view detail) {
    return {
        {"schema", "redclaw.debug-bridge-control.v1"},
        {"request_id", request_id},
        {"ok", false},
        {"error_code", code},
        {"error_detail", detail},
    };
}

bool is_no_argument_action(std::string_view action) {
    static const std::set<std::string_view> actions = {
        "status", "reconnect", "stop", "remote_log_snapshot",
        "agent_status", "agent_interrupt", "agent_follow_up_fixture",
        "agent_evidence", "export_evidence", "remote_control_start",
        "remote_control_pause",
    };
    return actions.contains(action);
}

std::optional<std::string> build_redclaw_debug_request(
    std::string_view request_id,
    std::string_view action,
    const boost::json::object& arguments,
    std::string* error) {
    boost::json::object request;
    request["schema"] = "redclaw.debug-control.v1";
    request["request_id"] = request_id;
    request["action"] = action;
    if (is_no_argument_action(action)) {
        if (!arguments.empty()) {
            *error = "action does not accept arguments";
            return std::nullopt;
        }
    } else if (action == "start") {
        const auto* role = arguments.if_contains("role");
        if (role == nullptr || !role->is_string()
            || (role->as_string() != "host" && role->as_string() != "controller")
            || arguments.size() != 1U) {
            *error = "start requires only role=host|controller";
            return std::nullopt;
        }
        request["role"] = role->as_string();
    } else if (action == "tail_log" || action == "remote_log_read") {
        const auto* limit = arguments.if_contains("limit");
        if (limit == nullptr || !limit->is_int64()
            || limit->as_int64() < 1 || limit->as_int64() > 200
            || arguments.size() != 1U) {
            *error = "log action requires only limit within 1..200";
            return std::nullopt;
        }
        request["limit"] = limit->as_int64();
    } else if (action == "agent_start_fixture") {
        const auto* fixture = arguments.if_contains("fixture_id");
        const auto* provider = arguments.if_contains("provider");
        if (fixture == nullptr || !fixture->is_string()
            || (fixture->as_string() != "inspect_project"
                && fixture->as_string() != "write_marker_and_test")
            || provider == nullptr || !provider->is_string()
            || (provider->as_string() != "codex" && provider->as_string() != "cursor")
            || arguments.size() != 2U) {
            *error = "agent fixture arguments are invalid";
            return std::nullopt;
        }
        request["fixture_id"] = fixture->as_string();
        request["provider"] = provider->as_string();
    } else if (action == "agent_approval") {
        const auto* decision = arguments.if_contains("decision");
        if (decision == nullptr || !decision->is_string()
            || (decision->as_string() != "accept" && decision->as_string() != "reject")
            || arguments.size() != 1U) {
            *error = "agent approval requires only accept|reject";
            return std::nullopt;
        }
        request["decision"] = decision->as_string();
    } else {
        *error = "remote control action is not allowed by the debug bridge";
        return std::nullopt;
    }
    error->clear();
    return boost::json::serialize(request);
}

bool key_contains(std::string_view key, std::string_view fragment) {
    return key.find(fragment) != std::string_view::npos;
}

bool contains_sensitive_runtime_text(std::string_view text) {
    if (text.starts_with("\\\\")
        || text.find("candidate:") != std::string_view::npos
        || text.find("a=ice-") != std::string_view::npos
        || text.find("fingerprint:") != std::string_view::npos
        || text.find("passphrase") != std::string_view::npos
        || text.find("authorization:") != std::string_view::npos
        || text.find("token=") != std::string_view::npos) {
        return true;
    }
    for (std::size_t index = 0; index + 2U < text.size(); ++index) {
        if (std::isalpha(static_cast<unsigned char>(text[index])) != 0
            && text[index + 1U] == ':'
            && (text[index + 2U] == '\\' || text[index + 2U] == '/')) {
            return true;
        }
    }
    return false;
}

void sanitize_remote_json(boost::json::value* value, std::string_view key = {}) {
    if (value->is_object()) {
        for (auto& field : value->as_object()) {
            sanitize_remote_json(
                &field.value(),
                std::string_view(field.key().data(), field.key().size()));
        }
        return;
    }
    if (value->is_array()) {
        for (auto& item : value->as_array()) {
            sanitize_remote_json(&item, key);
        }
        return;
    }
    if (!value->is_string()) {
        return;
    }

    const std::string_view text(value->as_string().data(), value->as_string().size());
    if (key_contains(key, "passphrase")
        || key_contains(key, "password")
        || key_contains(key, "credential")
        || key_contains(key, "account")
        || key_contains(key, "token")) {
        *value = "[redacted-secret]";
    } else if (key_contains(key, "address") || key_contains(key, "candidate")) {
        *value = "[redacted-network-address]";
    } else if (key_contains(key, "path")) {
        const std::filesystem::path path{std::string(text)};
        const std::string filename = path.filename().string();
        *value = filename.empty() ? "[redacted-local-path]" : filename;
    } else if (contains_sensitive_runtime_text(text)) {
        *value = "[redacted-sensitive-diagnostic-text]";
    }
}

std::string sanitize_remote_response(std::string_view response) {
    boost::system::error_code parse_error;
    boost::json::value parsed = boost::json::parse(response, parse_error);
    if (parse_error || !parsed.is_object()) {
        return boost::json::serialize(make_local_error(
            "remote", "invalid_redclaw_response", "RedClaw returned malformed JSON"));
    }
    sanitize_remote_json(&parsed);
    return boost::json::serialize(parsed);
}

#if defined(_WIN32)
std::wstring widen_ascii(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

std::string invoke_named_pipe(
    const std::string& pipe_name,
    std::string_view request,
    std::uint32_t timeout_ms) {
    const std::wstring path = L"\\\\.\\pipe\\" + widen_ascii(pipe_name);
    if (WaitNamedPipeW(path.c_str(), timeout_ms) == FALSE) {
        return boost::json::serialize(make_local_error("remote", "redclaw_pipe_unavailable", "RedClaw debug control pipe is unavailable"));
    }
    HANDLE pipe = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return boost::json::serialize(make_local_error("remote", "redclaw_pipe_open_failed", "failed to open RedClaw debug control pipe"));
    }
    std::string line(request);
    line.push_back('\n');
    DWORD written = 0;
    const bool sent = WriteFile(
        pipe, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) != FALSE
        && written == line.size();
    std::string response;
    if (sent) {
        std::array<char, 4096> buffer{};
        while (response.size() < kMaxLocalControlBytes) {
            DWORD read = 0;
            if (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) == FALSE
                || read == 0) {
                break;
            }
            response.append(buffer.data(), read);
            const std::size_t newline = response.find('\n');
            if (newline != std::string::npos) {
                response.resize(newline);
                break;
            }
        }
    }
    CloseHandle(pipe);
    if (!sent || response.empty()) {
        return boost::json::serialize(make_local_error("remote", "redclaw_pipe_io_failed", "RedClaw debug control pipe returned no response"));
    }
    return response;
}

class LocalPipeServer final {
public:
    using Handler = std::function<std::string(std::string)>;

    LocalPipeServer(std::string name, Handler handler)
        : name_(std::move(name)), handler_(std::move(handler)) {}

    ~LocalPipeServer() { stop(); }

    bool start() {
        if (thread_.joinable()) {
            return false;
        }
        stopping_.store(false);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        stopping_.store(true);
        const std::wstring path = L"\\\\.\\pipe\\" + widen_ascii(name_);
        HANDLE wake = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (wake != INVALID_HANDLE_VALUE) {
            CloseHandle(wake);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void run() {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr) != FALSE) {
            attributes.lpSecurityDescriptor = descriptor;
        }
        while (!stopping_.load()) {
            const std::wstring path = L"\\\\.\\pipe\\" + widen_ascii(name_);
            HANDLE pipe = CreateNamedPipeW(
                path.c_str(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1,
                static_cast<DWORD>(kMaxLocalControlBytes),
                static_cast<DWORD>(kMaxLocalControlBytes),
                1000,
                attributes.lpSecurityDescriptor == nullptr ? nullptr : &attributes);
            if (pipe == INVALID_HANDLE_VALUE) {
                std::this_thread::sleep_for(250ms);
                continue;
            }
            const BOOL connected = ConnectNamedPipe(pipe, nullptr)
                ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (connected != FALSE && !stopping_.load()) {
                std::string request;
                std::array<char, 4096> buffer{};
                while (request.size() < kMaxLocalControlBytes) {
                    DWORD read = 0;
                    if (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) == FALSE
                        || read == 0) {
                        break;
                    }
                    request.append(buffer.data(), read);
                    const std::size_t newline = request.find('\n');
                    if (newline != std::string::npos) {
                        request.resize(newline);
                        break;
                    }
                }
                std::string response = handler_(std::move(request));
                response.push_back('\n');
                DWORD written = 0;
                (void)WriteFile(
                    pipe,
                    response.data(),
                    static_cast<DWORD>((std::min)(response.size(), kMaxLocalControlBytes)),
                    &written,
                    nullptr);
                FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
        if (descriptor != nullptr) {
            LocalFree(descriptor);
        }
    }

    std::string name_;
    Handler handler_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};
#endif

class DebugBridgeRuntime final {
public:
    DebugBridgeRuntime(BridgeOptions options, std::string passphrase)
        : options_(std::move(options)), passphrase_(std::move(passphrase)) {}

    int run() {
        apply_route_cache();
        configure_callbacks();

        redclaw::net::IceGatheringConfig config;
        config.ice_servers = options_.ice_servers;
        config.bind_address = options_.bind_address;
        config.enable_ice_tcp = false;
        config.initiate_offer = options_.role == BridgeRole::kHost;
        config.data_channels = {redclaw::net::DataChannelKind::kDebugBridge};
        if (cached_preferred_port_ != 0) {
            config.port_range_begin = static_cast<std::uint16_t>((std::max)(1024, static_cast<int>(cached_preferred_port_) - 8));
            config.port_range_end = static_cast<std::uint16_t>((std::min)(65535, static_cast<int>(cached_preferred_port_) + 8));
        }

        std::string error;
        if (!ice_.startGathering(config, &error)) {
            std::cerr << "[debug-bridge] failed to start ICE: " << error << '\n';
            return 2;
        }

        initialize_agent_broker();
#if defined(_WIN32)
        LocalPipeServer local_server(options_.local_control_name, [this](std::string request) {
            return handle_local_request(std::move(request));
        });
        if (!local_server.start()) {
            std::cerr << "[debug-bridge] failed to start local control pipe\n";
            return 3;
        }
#endif
        std::cout << "[debug-bridge] role=" << role_name(options_.role)
                  << " bridge_id=" << options_.bridge_id
                  << " control=" << options_.local_control_name
                  << " route_cache=" << (route_cache_hit_ ? "hit" : "miss") << '\n';

        const auto started = std::chrono::steady_clock::now();
        auto next_heartbeat = started;
        while (!stopping_.load()) {
            if (options_.run_seconds > 0
                && std::chrono::steady_clock::now() - started
                    >= std::chrono::seconds(options_.run_seconds)) {
                break;
            }
            publish_local_signal_if_needed();
            read_remote_signal();
            drain_inbound();
            drain_local_requests();
            drain_agent_outbound();
            if (agent_broker_ != nullptr) {
                agent_broker_->tick();
            }
            if (channel_open_.load() && std::chrono::steady_clock::now() >= next_heartbeat) {
                if (hello_pending_.exchange(false)) {
                    send_message(redclaw::protocol::DebugBridgeMessageTypeV1::kHello);
                }
                send_message(redclaw::protocol::DebugBridgeMessageTypeV1::kHeartbeat);
                next_heartbeat = std::chrono::steady_clock::now() + 2s;
            }
            if (connected_.load() && !route_cache_saved_) {
                persist_route_cache();
            }
            std::this_thread::sleep_for(50ms);
        }

        if (agent_broker_ != nullptr) {
            agent_broker_->disconnect();
            agent_broker_->shutdown();
        }
        ice_.close();
#if defined(_WIN32)
        local_server.stop();
#endif
        return 0;
    }

private:
    void apply_route_cache() {
        const auto cache = load_route_cache(
            options_.route_cache_file, options_.network_fingerprint);
        if (!cache.has_value()) {
            return;
        }
        route_cache_hit_ = true;
        cached_preferred_port_ = cache->preferred_port;
        if (options_.bind_address.empty()) {
            options_.bind_address = cache->bind_address;
        }
        if (options_.ice_servers.empty()) {
            options_.ice_servers = cache->ice_servers;
        }
    }

    void configure_callbacks() {
        ice_.onConnectionStateChanged([this](redclaw::net::IceConnectionState state) {
            connected_.store(state == redclaw::net::IceConnectionState::kConnected);
            if (state == redclaw::net::IceConnectionState::kFailed
                || state == redclaw::net::IceConnectionState::kClosed) {
                channel_open_.store(false);
            }
        });
        ice_.onLocalDescription([this](const std::string& sdp, bool is_offer) {
            std::lock_guard lock(signal_mutex_);
            local_sdp_ = sdp;
            local_is_offer_ = is_offer;
            local_signal_dirty_ = true;
        });
        ice_.onLocalCandidate([this](const std::string& candidate, const std::string& mid) {
            std::lock_guard lock(signal_mutex_);
            const std::string line = mid + '\t' + candidate;
            if (std::find(local_candidates_.begin(), local_candidates_.end(), line)
                == local_candidates_.end()) {
                local_candidates_.push_back(line);
                local_signal_dirty_ = true;
            }
        });
        ice_.onDataChannelOpen([this](redclaw::net::DataChannelKind kind) {
            if (kind != redclaw::net::DataChannelKind::kDebugBridge) {
                return;
            }
            channel_open_.store(true);
            hello_pending_.store(true);
            if (agent_broker_ != nullptr) {
                agent_broker_->connect(options_.session_epoch);
            }
        });
        ice_.onDataChannelClosed([this](redclaw::net::DataChannelKind kind) {
            if (kind == redclaw::net::DataChannelKind::kDebugBridge) {
                channel_open_.store(false);
                if (agent_broker_ != nullptr) {
                    agent_broker_->disconnect();
                }
            }
        });
        ice_.onDataChannelBinaryMessage(
            [this](redclaw::net::DataChannelKind kind, std::span<const std::uint8_t> bytes) {
                if (kind != redclaw::net::DataChannelKind::kDebugBridge) {
                    return;
                }
                const auto parsed = redclaw::protocol::parse_debug_bridge_message_v1(
                    {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
                if (!parsed.ok || parsed.value.bridge_id != options_.bridge_id) {
                    return;
                }
                std::string guard_error;
                std::lock_guard lock(inbound_mutex_);
                if (!incoming_guard_.accept(parsed.value, &guard_error)) {
                    return;
                }
                last_remote_sequence_ = parsed.value.sequence;
                inbound_.push_back({parsed.value});
            });
    }

    void initialize_agent_broker() {
        if (options_.role != BridgeRole::kHost || !options_.allow_agent_tasks) {
            return;
        }
        agent_broker_ = std::make_unique<redclaw::agent::RemoteAgentBroker>(
            redclaw::agent::RemoteAgentBrokerConfig{
                .authorized = true,
                .metadata_path = options_.agent_project_manifest.parent_path()
                    / "debug-bridge-agent-task-metadata-v1.frames",
            });
        std::vector<redclaw::agent::AgentProjectRegistration> projects;
        std::string error;
        if (!redclaw::agent::load_agent_project_manifest(
                options_.agent_project_manifest, &projects, &error)) {
            std::cerr << "[debug-bridge] Agent authorization failed closed: " << error << '\n';
            agent_broker_->set_authorized(false);
        } else {
            for (auto& project : projects) {
                agent_broker_->add_project(std::move(project));
            }
        }
        auto codex = redclaw::agent::make_codex_app_server_provider();
        (void)codex->probe();
        agent_broker_->add_provider(std::move(codex));
        auto cursor = redclaw::agent::make_cursor_agent_provider();
        (void)cursor->probe();
        agent_broker_->add_provider(std::move(cursor));
    }

    std::filesystem::path local_signal_path() const {
        return options_.signal_directory
            / (options_.role == BridgeRole::kHost
                ? "debug-bridge-host.sealed" : "debug-bridge-controller.sealed");
    }

    std::filesystem::path remote_signal_path() const {
        return options_.signal_directory
            / (options_.role == BridgeRole::kHost
                ? "debug-bridge-controller.sealed" : "debug-bridge-host.sealed");
    }

    void set_signal_publish_error(std::string_view category) {
        bool changed = false;
        {
            std::lock_guard lock(signal_mutex_);
            changed = last_signal_publish_error_ != category;
            last_signal_publish_error_.assign(category);
        }
        if (changed) {
            std::cerr << "[debug-bridge] local signal publish blocked: "
                      << category << '\n';
        }
    }

    void publish_local_signal_if_needed() {
        std::string sdp;
        std::vector<std::string> candidates;
        bool is_offer = false;
        {
            std::lock_guard lock(signal_mutex_);
            if (!local_signal_dirty_ || local_sdp_.empty()) {
                return;
            }
            sdp = local_sdp_;
            candidates = local_candidates_;
            is_offer = local_is_offer_;
        }
        if (is_offer != (options_.role == BridgeRole::kHost)) {
            set_signal_publish_error("description_role_mismatch");
            return;
        }
        const auto attributes = redclaw::protocol::parse_sdp_signaling_attributes(sdp);
        if (!attributes.complete()) {
            set_signal_publish_error(attributes.first_missing_reason());
            return;
        }
        redclaw::protocol::OfferBlobV1 signal;
        signal.session_id = options_.bridge_id;
        signal.host_peer_id = "debug-bridge-host";
        signal.controller_peer_id = "debug-bridge-controller";
        signal.nonce = options_.session_epoch + "-" + role_name(options_.role) + "-v1";
        signal.created_at_ms = now_unix_ms();
        signal.description_sdp = std::move(sdp);
        signal.ice_ufrag = attributes.ice_ufrag;
        signal.ice_pwd = attributes.ice_pwd;
        signal.host_fingerprint = attributes.fingerprint;
        signal.trickle_ice = true;
        signal.candidates = std::move(candidates);
        const auto encrypted = redclaw::protocol::encrypt_offer_blob_v1(signal, passphrase_);
        std::string error;
        if (!encrypted.ok) {
            set_signal_publish_error("seal_failed");
            return;
        }
        if (!write_text_file_atomically(local_signal_path(), encrypted.value, &error)) {
            set_signal_publish_error("write_failed");
            return;
        }
        {
            std::lock_guard lock(signal_mutex_);
            if (local_sdp_ == signal.description_sdp
                && local_candidates_.size() == signal.candidates.size()) {
                local_signal_dirty_ = false;
            }
            last_signal_publish_error_.clear();
        }
        local_signal_publish_total_.fetch_add(1);
    }

    void read_remote_signal() {
        const std::string sealed = read_text_file(remote_signal_path());
        if (sealed.empty() || sealed == last_remote_signal_) {
            return;
        }
        const auto decrypted = redclaw::protocol::decrypt_offer_blob_v1(sealed, passphrase_);
        const BridgeRole remote_role = options_.role == BridgeRole::kHost
            ? BridgeRole::kController : BridgeRole::kHost;
        if (!decrypted.ok
            || decrypted.value.session_id != options_.bridge_id
            || decrypted.value.nonce
                != options_.session_epoch + "-" + role_name(remote_role) + "-v1"
            || decrypted.value.created_at_ms + 30ULL * 60ULL * 1000ULL < now_unix_ms()) {
            return;
        }
        const bool remote_is_offer = remote_role == BridgeRole::kHost;
        if (!remote_description_applied_.load()) {
            std::string error;
            if (!ice_.applyRemoteDescription(
                    decrypted.value.description_sdp, remote_is_offer, &error)) {
                std::cerr << "[debug-bridge] remote description rejected: " << error << '\n';
                return;
            }
            remote_description_applied_.store(true);
        }
        for (const auto& line : decrypted.value.candidates) {
            if (applied_remote_candidates_.contains(line)) {
                continue;
            }
            const std::size_t tab = line.find('\t');
            if (tab == std::string::npos) {
                continue;
            }
            std::string error;
            if (ice_.applyRemoteCandidate(line.substr(tab + 1U), line.substr(0, tab), &error)) {
                applied_remote_candidates_.insert(line);
            }
        }
        last_remote_signal_ = sealed;
    }

    redclaw::protocol::DebugBridgeEnvelopeV1 make_envelope(
        redclaw::protocol::DebugBridgeMessageTypeV1 type) {
        redclaw::protocol::DebugBridgeEnvelopeV1 message;
        message.bridge_id = options_.bridge_id;
        message.session_epoch = options_.session_epoch;
        message.message_id = next_message_id_.fetch_add(1);
        message.sequence = next_sequence_.fetch_add(1);
        message.acknowledged_sequence = last_remote_sequence_.load();
        message.sent_at_ms = now_unix_ms();
        message.type = type;
        return message;
    }

    bool send_envelope(const redclaw::protocol::DebugBridgeEnvelopeV1& message) {
        std::string validation_error;
        if (!redclaw::protocol::validate_debug_bridge_message_v1(message, &validation_error)) {
            return false;
        }
        std::string error;
        const auto serialized = redclaw::protocol::serialize_debug_bridge_message_v1(message);
        if (serialized.empty()) return false;
        return ice_.sendDataChannelBinaryMessage(
            redclaw::net::DataChannelKind::kDebugBridge,
            {reinterpret_cast<const std::uint8_t*>(serialized.data()), serialized.size()},
            &error);
    }

    void send_message(redclaw::protocol::DebugBridgeMessageTypeV1 type) {
        if (!channel_open_.load()) {
            return;
        }
        auto message = make_envelope(type);
        message.ok = true;
        message.payload = role_name(options_.role);
        (void)send_envelope(message);
    }

    void drain_inbound() {
        std::deque<InboundItem> items;
        {
            std::lock_guard lock(inbound_mutex_);
            items.swap(inbound_);
        }
        for (auto& item : items) {
            auto& message = item.envelope;
            if (message.type == redclaw::protocol::DebugBridgeMessageTypeV1::kCommandRequest
                && options_.role == BridgeRole::kHost) {
                handle_remote_command(message);
            } else if (message.type == redclaw::protocol::DebugBridgeMessageTypeV1::kCommandResult
                || message.type == redclaw::protocol::DebugBridgeMessageTypeV1::kStatusResponse
                || message.type == redclaw::protocol::DebugBridgeMessageTypeV1::kError) {
                complete_pending(message.request_id, message.payload);
            } else if (message.type == redclaw::protocol::DebugBridgeMessageTypeV1::kStatusRequest) {
                auto response = make_envelope(redclaw::protocol::DebugBridgeMessageTypeV1::kStatusResponse);
                response.request_id = message.request_id;
                response.action = "bridge_status";
                response.ok = true;
                response.payload = boost::json::serialize(status_object(message.request_id));
                (void)send_envelope(response);
            } else if (message.type == redclaw::protocol::DebugBridgeMessageTypeV1::kAgentFrame) {
                handle_agent_frame(message.payload);
            }
        }
    }

    void handle_remote_command(const redclaw::protocol::DebugBridgeEnvelopeV1& message) {
        boost::system::error_code parse_error;
        const boost::json::value parsed = boost::json::parse(message.payload, parse_error);
        std::string validation_error;
        std::optional<std::string> request;
        if (!parse_error && parsed.is_object()) {
            request = build_redclaw_debug_request(
                message.request_id, message.action, parsed.as_object(), &validation_error);
        } else {
            validation_error = "command arguments must be a JSON object";
        }
        std::string result;
        if (!request.has_value()) {
            result = boost::json::serialize(make_local_error(
                message.request_id, "invalid_remote_command", validation_error));
        } else {
#if defined(_WIN32)
            result = sanitize_remote_response(
                invoke_named_pipe(options_.redclaw_control_name, *request, 60000));
#else
            result = boost::json::serialize(make_local_error(
                message.request_id, "platform_unsupported", "named-pipe relay requires Windows"));
#endif
        }
        if (result.size() > redclaw::protocol::kMaxDebugBridgePayloadBytes) {
            result = boost::json::serialize(make_local_error(
                message.request_id, "remote_response_too_large", "response exceeded the bounded bridge payload"));
        }
        auto response = make_envelope(redclaw::protocol::DebugBridgeMessageTypeV1::kCommandResult);
        response.request_id = message.request_id;
        response.action = message.action;
        response.ok = request.has_value();
        response.error_code = request.has_value() ? "none" : "invalid_remote_command";
        response.payload = std::move(result);
        (void)send_envelope(response);
    }

    void handle_agent_frame(const std::string& payload) {
        const auto parsed = redclaw::protocol::parse_agent_message_v1(payload);
        if (!parsed.ok) {
            return;
        }
        if (options_.role == BridgeRole::kHost) {
            if (agent_broker_ != nullptr) {
                std::string error;
                (void)agent_broker_->handle_message(parsed.value, &error);
            }
            return;
        }
        std::lock_guard lock(agent_events_mutex_);
        agent_events_.push_back(payload);
        while (agent_events_.size() > kMaxAgentEventRecords) {
            agent_events_.pop_front();
        }
        agent_event_count_.store(agent_events_.size());
    }

    void drain_agent_outbound() {
        if (agent_broker_ == nullptr || !channel_open_.load()) {
            return;
        }
        for (const auto& agent_message : agent_broker_->take_outbound(32)) {
            auto envelope = make_envelope(redclaw::protocol::DebugBridgeMessageTypeV1::kAgentFrame);
            envelope.payload = redclaw::protocol::serialize_agent_message_v1(agent_message);
            envelope.ok = true;
            (void)send_envelope(envelope);
        }
    }

    boost::json::object status_object(std::string_view request_id) {
        bool local_description_ready = false;
        bool local_description_is_offer = false;
        bool local_description_role_matches = false;
        bool local_has_ice_ufrag = false;
        bool local_has_ice_pwd = false;
        bool local_has_fingerprint = false;
        std::size_t local_candidate_count = 0;
        std::string signal_publish_error;
        std::string signal_publish_state = "waiting_for_description";
        {
            std::lock_guard lock(signal_mutex_);
            local_description_ready = !local_sdp_.empty();
            local_description_is_offer = local_is_offer_;
            local_description_role_matches = local_description_ready
                && local_is_offer_ == (options_.role == BridgeRole::kHost);
            const auto attributes =
                redclaw::protocol::parse_sdp_signaling_attributes(local_sdp_);
            local_has_ice_ufrag = !attributes.ice_ufrag.empty();
            local_has_ice_pwd = !attributes.ice_pwd.empty();
            local_has_fingerprint = !attributes.fingerprint.empty();
            local_candidate_count = local_candidates_.size();
            if (!last_signal_publish_error_.empty()) {
                signal_publish_error = last_signal_publish_error_;
                signal_publish_state = signal_publish_error;
            } else if (local_signal_publish_total_.load() > 0) {
                signal_publish_state = "published";
            } else if (local_description_ready) {
                signal_publish_state = "ready_to_publish";
            }
        }
        return {
            {"schema", "redclaw.debug-bridge-control.v1"},
            {"request_id", request_id},
            {"ok", true},
            {"role", role_name(options_.role)},
            {"ice_connected", connected_.load()},
            {"channel_open", channel_open_.load()},
            {"remote_description_applied", remote_description_applied_.load()},
            {"local_description_ready", local_description_ready},
            {"local_description_is_offer", local_description_is_offer},
            {"local_description_role_matches", local_description_role_matches},
            {"local_has_ice_ufrag", local_has_ice_ufrag},
            {"local_has_ice_pwd", local_has_ice_pwd},
            {"local_has_fingerprint", local_has_fingerprint},
            {"local_candidate_count", local_candidate_count},
            {"local_signal_publish_total", local_signal_publish_total_.load()},
            {"local_signal_publish_state", signal_publish_state},
            {"last_signal_publish_error", signal_publish_error},
            {"route_cache_hit", route_cache_hit_},
            {"agent_authorized", options_.allow_agent_tasks},
            {"agent_event_count", agent_event_count_.load()},
        };
    }

    redclaw::protocol::AgentMessageEnvelopeV1 build_agent_request(
        std::string_view action,
        const boost::json::object& request,
        std::string* error) {
        redclaw::protocol::AgentMessageEnvelopeV1 message;
        message.session_epoch = options_.session_epoch;
        message.message_id = next_agent_message_id_++;
        message.sent_at_ms = now_unix_ms();
        const auto string_value = [&](std::string_view name) -> std::string {
            const auto* value = request.if_contains(name);
            return value != nullptr && value->is_string()
                ? std::string(value->as_string()) : std::string{};
        };
        message.task_id = string_value("task_id");
        message.request_id = string_value("approval_request_id");
        message.text = string_value("instruction");
        message.project_id = string_value("project_id");
        message.model = string_value("model");
        const std::string provider = string_value("provider");
        message.provider = provider == "cursor"
            ? redclaw::protocol::AgentProviderKindV1::kCursor
            : (provider == "codex"
                ? redclaw::protocol::AgentProviderKindV1::kCodex
                : redclaw::protocol::AgentProviderKindV1::kNone);
        const std::string mode = string_value("work_directory_mode");
        message.work_directory_mode = mode == "direct_workspace"
            ? redclaw::protocol::AgentWorkDirectoryModeV1::kDirectWorkspace
            : redclaw::protocol::AgentWorkDirectoryModeV1::kIsolatedWorktree;
        if (action == "agent_task_create") {
            message.type = redclaw::protocol::AgentMessageTypeV1::kTaskCreate;
        } else if (action == "agent_turn_start") {
            message.type = redclaw::protocol::AgentMessageTypeV1::kTurnStart;
        } else if (action == "agent_turn_steer") {
            message.type = redclaw::protocol::AgentMessageTypeV1::kTurnSteer;
        } else if (action == "agent_turn_interrupt") {
            message.type = redclaw::protocol::AgentMessageTypeV1::kTurnInterrupt;
        } else if (action == "agent_task_sync") {
            message.type = redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest;
        } else if (action == "agent_approval") {
            message.type = redclaw::protocol::AgentMessageTypeV1::kApprovalDecision;
            const std::string decision = string_value("decision");
            message.approval_decision = decision == "accept"
                ? redclaw::protocol::AgentApprovalDecisionV1::kAccept
                : (decision == "reject"
                    ? redclaw::protocol::AgentApprovalDecisionV1::kReject
                    : redclaw::protocol::AgentApprovalDecisionV1::kNone);
        } else {
            *error = "unknown Agent action";
            return {};
        }
        if (!redclaw::protocol::validate_agent_message_v1(message, error)) {
            return {};
        }
        return message;
    }

    std::string handle_local_request(std::string request_text) {
        boost::system::error_code parse_error;
        const boost::json::value parsed = boost::json::parse(request_text, parse_error);
        if (parse_error || !parsed.is_object()) {
            return boost::json::serialize(make_local_error("unknown", "invalid_json", "request must be a JSON object"));
        }
        const auto& request = parsed.as_object();
        const auto* schema = request.if_contains("schema");
        const auto* request_id_value = request.if_contains("request_id");
        const auto* action_value = request.if_contains("action");
        if (schema == nullptr || !schema->is_string()
            || schema->as_string() != "redclaw.debug-bridge-control.v1"
            || request_id_value == nullptr || !request_id_value->is_string()
            || action_value == nullptr || !action_value->is_string()) {
            return boost::json::serialize(make_local_error("unknown", "invalid_envelope", "schema, request_id, and action are required"));
        }
        const std::string request_id(request_id_value->as_string());
        const std::string action(action_value->as_string());
        if (!valid_token(request_id, 128) || !valid_token(action, 64)) {
            return boost::json::serialize(make_local_error(request_id, "invalid_token", "request identity is invalid"));
        }
        if (action == "bridge_status") {
            return boost::json::serialize(status_object(request_id));
        }
        if (action == "agent_events") {
            boost::json::array events;
            std::lock_guard lock(agent_events_mutex_);
            for (const auto& event : agent_events_) {
                events.emplace_back(event);
            }
            auto response = status_object(request_id);
            response["events"] = std::move(events);
            return boost::json::serialize(response);
        }
        if (!channel_open_.load()) {
            return boost::json::serialize(make_local_error(request_id, "bridge_not_connected", "debug bridge DataChannel is not open"));
        }

        const auto* arguments_value = request.if_contains("arguments");
        const boost::json::object empty_arguments;
        const boost::json::object* arguments = &empty_arguments;
        if (arguments_value != nullptr) {
            if (!arguments_value->is_object()) {
                return boost::json::serialize(make_local_error(request_id, "invalid_arguments", "arguments must be an object"));
            }
            arguments = &arguments_value->as_object();
        }

        auto pending = std::make_shared<PendingResponse>();
        LocalRequest local;
        local.pending = pending;
        if (std::string_view(action).starts_with("agent_")) {
            std::string error;
            const auto agent_message = build_agent_request(action, *arguments, &error);
            if (!error.empty()) {
                return boost::json::serialize(make_local_error(request_id, "invalid_agent_request", error));
            }
            local.envelope = make_envelope(redclaw::protocol::DebugBridgeMessageTypeV1::kAgentFrame);
            local.envelope.request_id = request_id;
            local.envelope.payload = redclaw::protocol::serialize_agent_message_v1(agent_message);
            local.envelope.ok = true;
            local.complete_after_send = true;
        } else if (action == "remote_bridge_status") {
            local.envelope = make_envelope(redclaw::protocol::DebugBridgeMessageTypeV1::kStatusRequest);
            local.envelope.request_id = request_id;
            local.envelope.action = "bridge_status";
        } else {
            std::string validation_error;
            if (!build_redclaw_debug_request(request_id, action, *arguments, &validation_error).has_value()) {
                return boost::json::serialize(make_local_error(request_id, "invalid_remote_command", validation_error));
            }
            local.envelope = make_envelope(redclaw::protocol::DebugBridgeMessageTypeV1::kCommandRequest);
            local.envelope.request_id = request_id;
            local.envelope.action = action;
            local.envelope.payload = boost::json::serialize(*arguments);
        }
        {
            std::lock_guard lock(local_requests_mutex_);
            local_requests_.push_back(std::move(local));
        }
        std::unique_lock wait_lock(pending->mutex);
        if (!pending->cv.wait_for(wait_lock, 90s, [&] { return pending->complete; })) {
            return boost::json::serialize(make_local_error(request_id, "response_timeout", "remote bridge response timed out"));
        }
        return pending->response;
    }

    void drain_local_requests() {
        std::deque<LocalRequest> requests;
        {
            std::lock_guard lock(local_requests_mutex_);
            requests.swap(local_requests_);
        }
        for (auto& request : requests) {
            if (!send_envelope(request.envelope)) {
                set_pending_response(
                    request.pending,
                    boost::json::serialize(make_local_error(
                        request.envelope.request_id,
                        "bridge_send_failed",
                        "debug bridge send failed")));
                continue;
            }
            if (request.complete_after_send) {
                boost::json::object response{
                    {"schema", "redclaw.debug-bridge-control.v1"},
                    {"request_id", request.envelope.request_id},
                    {"ok", true},
                    {"accepted", true},
                };
                set_pending_response(request.pending, boost::json::serialize(response));
            } else {
                std::lock_guard lock(pending_mutex_);
                pending_[request.envelope.request_id] = request.pending;
            }
        }
    }

    static void set_pending_response(
        const std::shared_ptr<PendingResponse>& pending,
        std::string response) {
        {
            std::lock_guard lock(pending->mutex);
            pending->response = std::move(response);
            pending->complete = true;
        }
        pending->cv.notify_all();
    }

    void complete_pending(const std::string& request_id, const std::string& response) {
        std::shared_ptr<PendingResponse> pending;
        {
            std::lock_guard lock(pending_mutex_);
            const auto found = pending_.find(request_id);
            if (found == pending_.end()) {
                return;
            }
            pending = found->second;
            pending_.erase(found);
        }
        set_pending_response(pending, response);
    }

    void persist_route_cache() {
        RouteCacheRecord record;
        record.network_fingerprint = options_.network_fingerprint;
        record.bind_address = options_.bind_address;
        record.ice_servers = options_.ice_servers;
        record.expires_at_ms = now_unix_ms() + kRouteCacheLifetimeMs;
        {
            std::lock_guard lock(signal_mutex_);
            for (const auto& candidate : local_candidates_) {
                const auto port = candidate_port(candidate);
                if (port.has_value()) {
                    record.preferred_port = *port;
                    break;
                }
            }
        }
        route_cache_saved_ = save_route_cache(options_.route_cache_file, record);
    }

    BridgeOptions options_;
    std::string passphrase_;
    redclaw::net::IceConnectivityWrapper ice_;
    std::unique_ptr<redclaw::agent::RemoteAgentBroker> agent_broker_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> channel_open_{false};
    std::atomic<std::uint64_t> last_remote_sequence_{0};
    std::atomic<std::uint64_t> next_message_id_{1};
    std::atomic<std::uint64_t> next_sequence_{1};
    std::atomic<std::uint64_t> local_signal_publish_total_{0};
    std::uint64_t next_agent_message_id_ = 1;

    std::mutex signal_mutex_;
    std::string local_sdp_;
    bool local_is_offer_ = false;
    bool local_signal_dirty_ = false;
    std::vector<std::string> local_candidates_;
    std::string last_signal_publish_error_;
    std::atomic<bool> remote_description_applied_{false};
    std::set<std::string> applied_remote_candidates_;
    std::string last_remote_signal_;

    std::mutex inbound_mutex_;
    std::deque<InboundItem> inbound_;
    redclaw::protocol::DebugBridgeEpochGuardV1 incoming_guard_;
    std::mutex local_requests_mutex_;
    std::deque<LocalRequest> local_requests_;
    std::mutex pending_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PendingResponse>> pending_;
    mutable std::mutex agent_events_mutex_;
    std::deque<std::string> agent_events_;
    std::atomic<std::uint64_t> agent_event_count_{0};
    std::atomic<bool> hello_pending_{false};

    bool route_cache_hit_ = false;
    bool route_cache_saved_ = false;
    std::uint16_t cached_preferred_port_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    BridgeOptions options;
    std::string error;
    if (!parse_options(argc, argv, &options, &error)) {
        if (!error.empty()) {
            std::cerr << "[debug-bridge] " << error << '\n';
            print_usage();
            return 1;
        }
        return 0;
    }
    const std::string passphrase = read_text_file(options.passphrase_file);
    if (passphrase.size() < 16U || passphrase.size() > 1024U) {
        std::cerr << "[debug-bridge] passphrase file must contain 16..1024 bytes\n";
        return 1;
    }
    return DebugBridgeRuntime(std::move(options), passphrase).run();
}
