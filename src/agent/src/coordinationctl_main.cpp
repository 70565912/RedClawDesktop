#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "redclaw/agent/coordination.h"

namespace {

struct Options {
    std::filesystem::path journal_path;
    std::string git_sha;
    std::string executable_sha256;
    std::optional<redclaw::agent::CoordinationAuthorityV1> expected;
    std::optional<redclaw::agent::CoordinationAuthorityV1> next;
    std::string reason;
    bool explicit_selection = false;
    bool normal_agent_available = true;
    bool debug_bridge_available = true;
    bool status_only = false;
};

std::optional<redclaw::agent::CoordinationAuthorityV1> parse_authority(
    std::string_view value) {
    using Authority = redclaw::agent::CoordinationAuthorityV1;
    if (value == "none") {
        return Authority::kNone;
    }
    if (value == "normal_agent") {
        return Authority::kNormalAgent;
    }
    if (value == "debug_bridge") {
        return Authority::kDebugBridge;
    }
    if (value == "out_of_band") {
        return Authority::kOutOfBand;
    }
    return std::nullopt;
}

void print_usage() {
    std::cerr
        << "Usage:\n"
        << "  redclaw_coordinationctl --journal <path> --status\n"
        << "  redclaw_coordinationctl --journal <path> --git-sha <sha> "
           "--exe-sha256 <sha> --expected <authority> --next <authority> "
           "--reason <token> [--explicit] [--normal-agent-unavailable] "
           "[--debug-bridge-unavailable]\n";
}

bool take_value(int argc, char** argv, int* index, std::string* value) {
    if (*index + 1 >= argc) {
        return false;
    }
    *value = argv[++(*index)];
    return true;
}

bool parse_options(int argc, char** argv, Options* options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view name(argv[index]);
        std::string value;
        if (name == "--journal") {
            if (!take_value(argc, argv, &index, &value)) {
                return false;
            }
            options->journal_path = std::filesystem::path(value);
        } else if (name == "--git-sha") {
            if (!take_value(argc, argv, &index, &options->git_sha)) {
                return false;
            }
        } else if (name == "--exe-sha256") {
            if (!take_value(argc, argv, &index, &options->executable_sha256)) {
                return false;
            }
        } else if (name == "--expected" || name == "--next") {
            if (!take_value(argc, argv, &index, &value)) {
                return false;
            }
            const auto authority = parse_authority(value);
            if (!authority.has_value()) {
                return false;
            }
            if (name == "--expected") {
                options->expected = authority;
            } else {
                options->next = authority;
            }
        } else if (name == "--reason") {
            if (!take_value(argc, argv, &index, &options->reason)) {
                return false;
            }
        } else if (name == "--explicit") {
            options->explicit_selection = true;
        } else if (name == "--normal-agent-unavailable") {
            options->normal_agent_available = false;
        } else if (name == "--debug-bridge-unavailable") {
            options->debug_bridge_available = false;
        } else if (name == "--status") {
            options->status_only = true;
        } else {
            return false;
        }
    }
    if (options->journal_path.empty()) {
        return false;
    }
    if (options->status_only) {
        options->git_sha = "0000000";
        options->executable_sha256 = std::string(64, '0');
        return !options->expected.has_value() && !options->next.has_value();
    }
    return options->expected.has_value() && options->next.has_value()
        && !options->git_sha.empty() && !options->executable_sha256.empty()
        && !options->reason.empty();
}

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        print_usage();
        return 2;
    }

    redclaw::agent::NormalAgentControlStateV1 state({
        .journal_path = options.journal_path,
        .git_sha = options.git_sha,
        .executable_sha256 = options.executable_sha256,
    });
    std::string error;
    if (!state.initialize(&error)) {
        std::cerr << "coordination journal rejected: " << error << '\n';
        return 1;
    }
    if (options.status_only) {
        std::cout << "authority=" << redclaw::agent::to_string(state.authority())
                  << " sync_required=" << (state.sync_required() ? "true" : "false")
                  << '\n';
        return 0;
    }

    if (!state.transfer_authority(
            *options.expected,
            *options.next,
            options.explicit_selection,
            options.normal_agent_available,
            options.debug_bridge_available,
            options.reason,
            now_ms(),
            &error)) {
        std::cerr << "authority transfer rejected: " << error << '\n';
        return 1;
    }
    std::cout << "authority=" << redclaw::agent::to_string(state.authority())
              << " result=transferred\n";
    return 0;
}
