#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

namespace {

struct Options {
    std::string bind_ip = "0.0.0.0";
    std::uint16_t listen_port = 45907;
    std::string session_id = "m07-dual-session";
    std::string operator_id = "m07-dual-operator";
    std::string device_id = "m07-dual-device";
    std::string scenario = "timeout";
    std::string output_json_path;
    bool help_requested = false;
};

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += ch;
            break;
        }
    }
    return out;
}

std::uint64_t now_unix_seconds() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

bool read_value_arg(const std::vector<std::string>& args, std::size_t& index, std::string* out) {
    if (index + 1 >= args.size()) {
        return false;
    }
    ++index;
    *out = args[index];
    return true;
}

void print_usage() {
    std::cout
        << "Usage: redclaw_m07_dual_machine_probe_a [options]\n"
        << "Options:\n"
        << "  --bind-ip <ip>            Bind ip (default: 0.0.0.0)\n"
        << "  --listen-port <port>      Listen port (default: 45907)\n"
        << "  --session-id <id>         Session id\n"
        << "  --operator-id <id>        Operator id\n"
        << "  --device-id <id>          Device id\n"
        << "  --scenario <name>         timeout|unavailable|deny (default: timeout)\n"
        << "  --output-json-path <path> Optional local evidence output path\n"
        << "  --help                    Show help\n";
}

bool parse_port(std::string_view value, std::uint16_t* out) {
    if (value.empty()) {
        return false;
    }
    std::uint32_t parsed = 0;
    for (char ch : value) {
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

bool parse_options(const std::vector<std::string>& args, Options* options, std::string* error) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            options->help_requested = true;
            return true;
        }

        if (arg == "--bind-ip") {
            if (!read_value_arg(args, i, &options->bind_ip)) {
                *error = "missing value for --bind-ip";
                return false;
            }
            continue;
        }
        if (arg == "--listen-port") {
            std::string value;
            if (!read_value_arg(args, i, &value)) {
                *error = "missing value for --listen-port";
                return false;
            }
            if (!parse_port(value, &options->listen_port)) {
                *error = "invalid --listen-port: " + value;
                return false;
            }
            continue;
        }
        if (arg == "--session-id") {
            if (!read_value_arg(args, i, &options->session_id)) {
                *error = "missing value for --session-id";
                return false;
            }
            continue;
        }
        if (arg == "--operator-id") {
            if (!read_value_arg(args, i, &options->operator_id)) {
                *error = "missing value for --operator-id";
                return false;
            }
            continue;
        }
        if (arg == "--device-id") {
            if (!read_value_arg(args, i, &options->device_id)) {
                *error = "missing value for --device-id";
                return false;
            }
            continue;
        }
        if (arg == "--scenario") {
            if (!read_value_arg(args, i, &options->scenario)) {
                *error = "missing value for --scenario";
                return false;
            }
            continue;
        }
        if (arg == "--output-json-path") {
            if (!read_value_arg(args, i, &options->output_json_path)) {
                *error = "missing value for --output-json-path";
                return false;
            }
            continue;
        }

        *error = "unknown argument: " + arg;
        return false;
    }

    if (options->scenario != "timeout" && options->scenario != "unavailable" && options->scenario != "deny") {
        *error = "invalid --scenario value: " + options->scenario;
        return false;
    }
    return true;
}

std::string build_result_json(const Options& options, std::string_view client_ip) {
    const std::uint64_t now = now_unix_seconds();
    const std::string detail = options.scenario == "deny" ? "applied" : options.scenario;
    const std::string decision = options.scenario == "deny" ? "deny" : "blocked";
    const std::string error = options.scenario == "deny" ? "none" : "secure_desktop_unavailable";

    std::string json;
    json += "{\n";
    json += "  \"ok\": true,\n";
    json += "  \"runner\": \"program-a\",\n";
    json += "  \"session_id\": \"" + json_escape(options.session_id) + "\",\n";
    json += "  \"operator_id\": \"" + json_escape(options.operator_id) + "\",\n";
    json += "  \"device_id\": \"" + json_escape(options.device_id) + "\",\n";
    json += "  \"scenario\": \"" + json_escape(options.scenario) + "\",\n";
    json += "  \"client_ip\": \"" + json_escape(client_ip) + "\",\n";
    json += "  \"timestamp_unix\": " + std::to_string(now) + ",\n";
    json += "  \"audits\": [\n";
    json += "    {\"session_id\":\"" + json_escape(options.session_id) + "\",\"operator_id\":\"" + json_escape(options.operator_id) + "\",\"action\":\"request_privileged\",\"detail\":\"granted\",\"decision\":\"allow\",\"error\":\"none\",\"timestamp_unix\":" + std::to_string(now) + "},\n";
    json += "    {\"session_id\":\"" + json_escape(options.session_id) + "\",\"operator_id\":\"\",\"action\":\"uac_prompt_state\",\"detail\":\"" + json_escape(detail) + "\",\"decision\":\"" + json_escape(decision) + "\",\"error\":\"" + json_escape(error) + "\",\"timestamp_unix\":" + std::to_string(now) + "}\n";
    json += "  ],\n";
    json += "  \"error\": \"\"\n";
    json += "}\n";
    return json;
}

bool write_file(std::string_view path, std::string_view content, std::string* error) {
    std::ofstream out(std::string(path), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        *error = "failed to open output file: " + std::string(path);
        return false;
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out.good()) {
        *error = "failed to write output file: " + std::string(path);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    Options options;
    std::string parse_error;
    if (!parse_options(args, &options, &parse_error)) {
        std::cerr << "Error: " << parse_error << "\n\n";
        print_usage();
        return 2;
    }
    if (options.help_requested) {
        print_usage();
        return 0;
    }

    WSADATA wsadata;
    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
        std::cerr << "Error: WSAStartup failed\n";
        return 1;
    }

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) {
        std::cerr << "Error: socket() failed\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(options.listen_port);
    if (InetPtonA(AF_INET, options.bind_ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "Error: invalid bind ip: " << options.bind_ip << "\n";
        closesocket(server);
        WSACleanup();
        return 1;
    }

    if (bind(server, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "Error: bind() failed\n";
        closesocket(server);
        WSACleanup();
        return 1;
    }

    if (listen(server, 1) == SOCKET_ERROR) {
        std::cerr << "Error: listen() failed\n";
        closesocket(server);
        WSACleanup();
        return 1;
    }

    std::cout << "PROGRAM_A_READY ip=" << options.bind_ip << " port=" << options.listen_port << " scenario=" << options.scenario << "\n";

    sockaddr_in client_addr{};
    int client_len = sizeof(client_addr);
    SOCKET client = accept(server, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
    if (client == INVALID_SOCKET) {
        std::cerr << "Error: accept() failed\n";
        closesocket(server);
        WSACleanup();
        return 1;
    }

    char client_ip[INET_ADDRSTRLEN] = {0};
    InetNtopA(AF_INET, &client_addr.sin_addr, client_ip, static_cast<DWORD>(sizeof(client_ip)));

    char request_buffer[256] = {0};
    const int recv_len = recv(client, request_buffer, static_cast<int>(sizeof(request_buffer) - 1), 0);
    if (recv_len <= 0) {
        std::cerr << "Error: recv() failed\n";
        closesocket(client);
        closesocket(server);
        WSACleanup();
        return 1;
    }

    const std::string request(request_buffer, request_buffer + recv_len);
    if (request.find("GET_M07_RESULT") == std::string::npos) {
        std::cerr << "Error: unexpected request payload\n";
        closesocket(client);
        closesocket(server);
        WSACleanup();
        return 1;
    }

    const std::string result_json = build_result_json(options, client_ip);
    const int sent = send(client, result_json.c_str(), static_cast<int>(result_json.size()), 0);
    if (sent <= 0) {
        std::cerr << "Error: send() failed\n";
        closesocket(client);
        closesocket(server);
        WSACleanup();
        return 1;
    }

    if (!options.output_json_path.empty()) {
        std::string io_error;
        if (!write_file(options.output_json_path, result_json, &io_error)) {
            std::cerr << "Error: " << io_error << "\n";
            closesocket(client);
            closesocket(server);
            WSACleanup();
            return 1;
        }
    }

    std::cout << result_json;

    closesocket(client);
    closesocket(server);
    WSACleanup();
    return 0;
}
