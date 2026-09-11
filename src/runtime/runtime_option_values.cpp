#include "runtime_options_detail.h"
#include <cctype>
#include <fstream>

namespace redclaw::runtime {
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

bool append_ice_servers_from_file(
    const std::string& path,
    std::vector<std::string>* servers,
    std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        *error = "failed to open --ice-server-file: " + path;
        return false;
    }
    std::string line;
    std::size_t added = 0;
    while (std::getline(input, line)) {
        while (!line.empty() && (line.back() == '\r' || std::isspace(static_cast<unsigned char>(line.back())) != 0)) {
            line.pop_back();
        }
        std::size_t begin = 0;
        while (begin < line.size() && std::isspace(static_cast<unsigned char>(line[begin])) != 0) {
            ++begin;
        }
        const std::string value = line.substr(begin);
        if (value.empty() || value.front() == '#') {
            continue;
        }
        if (value.size() > 2048 || added >= 32) {
            *error = "--ice-server-file exceeds the supported entry limits";
            return false;
        }
        servers->push_back(value);
        ++added;
    }
    if (!input.eof()) {
        *error = "failed while reading --ice-server-file: " + path;
        return false;
    }
    if (added == 0) {
        *error = "--ice-server-file contains no ICE server entries";
        return false;
    }
    return true;
}

bool assign_runtime_role(std::string_view value, RuntimeOptions* options, std::string* error) {
    options->role_name = std::string(value);
    if (options->role_name == "host") {
        options->role = RuntimeRole::kHost;
        return true;
    }
    if (options->role_name == "controller") {
        options->role = RuntimeRole::kController;
        return true;
    }

    *error = "invalid --role value: " + options->role_name;
    return false;
}

}  // namespace redclaw::runtime
