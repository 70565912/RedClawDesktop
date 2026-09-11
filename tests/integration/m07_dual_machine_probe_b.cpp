#include <chrono>
#include <cstdint>
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
    std::string host = "127.0.0.1";
    std::uint16_t port = 45907;
    std::string output_json_path = "m07-b-result.json";
    bool help_requested = false;
};

bool read_value_arg(const std::vector<std::string>& args, std::size_t& index, std::string* out) {
    if (index + 1 >= args.size()) {
        return false;
    }
    ++index;
    *out = args[index];
    return true;
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

void print_usage() {
    std::cout
        << "Usage: redclaw_m07_dual_machine_probe_b [options]\n"
        << "Options:\n"
        << "  --host <ip>               Program A host ip (default: 127.0.0.1)\n"
        << "  --port <port>             Program A port (default: 45907)\n"
        << "  --output-json-path <path> Save fetched JSON result locally\n"
        << "  --help                    Show help\n";
}

bool parse_options(const std::vector<std::string>& args, Options* options, std::string* error) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];

        if (arg == "--help" || arg == "-h") {
            options->help_requested = true;
            return true;
        }
        if (arg == "--host") {
            if (!read_value_arg(args, i, &options->host)) {
                *error = "missing value for --host";
                return false;
            }
            continue;
        }
        if (arg == "--port") {
            std::string value;
            if (!read_value_arg(args, i, &value)) {
                *error = "missing value for --port";
                return false;
            }
            if (!parse_port(value, &options->port)) {
                *error = "invalid --port: " + value;
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

    return true;
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

std::uint64_t now_unix_seconds() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
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

    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) {
        std::cerr << "Error: socket() failed\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(options.port);
    if (InetPtonA(AF_INET, options.host.c_str(), &server_addr.sin_addr) != 1) {
        std::cerr << "Error: invalid host ip: " << options.host << "\n";
        closesocket(client);
        WSACleanup();
        return 1;
    }

    if (connect(client, reinterpret_cast<const sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "Error: connect() failed host=" << options.host << " port=" << options.port << "\n";
        closesocket(client);
        WSACleanup();
        return 1;
    }

    const std::string request = "GET_M07_RESULT\n";
    if (send(client, request.c_str(), static_cast<int>(request.size()), 0) <= 0) {
        std::cerr << "Error: send() failed\n";
        closesocket(client);
        WSACleanup();
        return 1;
    }

    std::string response;
    response.reserve(4096);
    char buffer[1024];
    while (true) {
        const int got = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            std::cerr << "Error: recv() failed\n";
            closesocket(client);
            WSACleanup();
            return 1;
        }
        response.append(buffer, buffer + got);
    }

    closesocket(client);
    WSACleanup();

    if (response.empty()) {
        std::cerr << "Error: empty response from Program A\n";
        return 1;
    }

    std::string io_error;
    if (!write_file(options.output_json_path, response, &io_error)) {
        std::cerr << "Error: " << io_error << "\n";
        return 1;
    }

    std::cout << "PROGRAM_B_OK host=" << options.host << " port=" << options.port << " timestamp_unix=" << now_unix_seconds() << "\n";
    std::cout << "PROGRAM_B_OUTPUT=" << options.output_json_path << "\n";
    std::cout << response;
    return 0;
}
