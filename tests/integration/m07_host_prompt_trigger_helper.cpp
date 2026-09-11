#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "redclaw/service/service_module.h"

namespace {

enum class PromptEventMode {
    kRaised,
    kClosed,
    kTimeout,
    kUnavailable,
};

struct Options {
    std::string session_id = "m07-trigger-session";
    std::string operator_id = "m07-trigger-operator";
    std::string device_id = "m07-trigger-device";
    std::string challenge_id = "m07-trigger-challenge";
    std::string signed_proof = "m07-trigger-proof";
    std::string reason_code = "m07_host_prompt_trigger";
    std::string uac_prompt_id = "m07-trigger-prompt";
    std::string unavailable_reason = "secure_desktop_unavailable";
    std::string output_json_path;
    PromptEventMode prompt_event = PromptEventMode::kRaised;
    std::string prompt_event_name = "raised";
    bool help_requested = false;
};

struct RunResult {
    bool ok = false;
    std::string error;
    redclaw::service::PrivilegedRequestResult grant_result;
    bool prompt_event_applied = false;
    redclaw::service::CapabilityLevel capability_after = redclaw::service::CapabilityLevel::kStandardControl;
    std::vector<redclaw::service::PrivilegedAuditEvent> audits;
    std::vector<redclaw::service::CapabilityChangeEvent> capability_changes;
};

void print_usage() {
    std::cout
        << "Usage: redclaw_m07_host_prompt_trigger_helper [options]\n"
        << "Options:\n"
        << "  --session-id <id>          Session id (default: m07-trigger-session)\n"
        << "  --operator-id <id>         Operator id (default: m07-trigger-operator)\n"
        << "  --device-id <id>           Device id (default: m07-trigger-device)\n"
        << "  --challenge-id <id>        Challenge id used for privileged request\n"
        << "  --signed-proof <value>     Signed proof used by step-up verifier\n"
        << "  --reason-code <value>      Request reason code\n"
        << "  --uac-prompt-id <id>       Prompt id (default: m07-trigger-prompt)\n"
        << "  --prompt-event <mode>      raised|closed|timeout|unavailable (default: raised)\n"
        << "  --unavailable-reason <v>   Reason for unavailable event\n"
        << "  --output-json-path <path>  Write JSON result file\n"
        << "  --help                     Show this help\n";
}

bool read_value_arg(const std::vector<std::string>& args, std::size_t& index, std::string* out) {
    if (index + 1 >= args.size()) {
        return false;
    }
    ++index;
    *out = args[index];
    return true;
}

PromptEventMode parse_prompt_event(std::string_view value) {
    if (value == "raised") {
        return PromptEventMode::kRaised;
    }
    if (value == "closed") {
        return PromptEventMode::kClosed;
    }
    if (value == "timeout") {
        return PromptEventMode::kTimeout;
    }
    if (value == "unavailable") {
        return PromptEventMode::kUnavailable;
    }
    return PromptEventMode::kRaised;
}

bool parse_options(const std::vector<std::string>& args, Options* options, std::string* error) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];

        if (arg == "--help" || arg == "-h") {
            options->help_requested = true;
            return true;
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

        if (arg == "--challenge-id") {
            if (!read_value_arg(args, i, &options->challenge_id)) {
                *error = "missing value for --challenge-id";
                return false;
            }
            continue;
        }

        if (arg == "--signed-proof") {
            if (!read_value_arg(args, i, &options->signed_proof)) {
                *error = "missing value for --signed-proof";
                return false;
            }
            continue;
        }

        if (arg == "--reason-code") {
            if (!read_value_arg(args, i, &options->reason_code)) {
                *error = "missing value for --reason-code";
                return false;
            }
            continue;
        }

        if (arg == "--uac-prompt-id") {
            if (!read_value_arg(args, i, &options->uac_prompt_id)) {
                *error = "missing value for --uac-prompt-id";
                return false;
            }
            continue;
        }

        if (arg == "--prompt-event") {
            if (!read_value_arg(args, i, &options->prompt_event_name)) {
                *error = "missing value for --prompt-event";
                return false;
            }

            if (options->prompt_event_name != "raised"
                && options->prompt_event_name != "closed"
                && options->prompt_event_name != "timeout"
                && options->prompt_event_name != "unavailable") {
                *error = "invalid --prompt-event value: " + options->prompt_event_name;
                return false;
            }

            options->prompt_event = parse_prompt_event(options->prompt_event_name);
            continue;
        }

        if (arg == "--unavailable-reason") {
            if (!read_value_arg(args, i, &options->unavailable_reason)) {
                *error = "missing value for --unavailable-reason";
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

    if (options->session_id.empty() || options->operator_id.empty() || options->device_id.empty()) {
        *error = "session/operator/device id cannot be empty";
        return false;
    }

    return true;
}

std::uint64_t now_unix_seconds() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

std::string escape_json(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
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

std::string to_string(redclaw::service::PrivilegedError error) {
    switch (error) {
    case redclaw::service::PrivilegedError::kNone:
        return "none";
    case redclaw::service::PrivilegedError::kInvalidSession:
        return "invalid_session";
    case redclaw::service::PrivilegedError::kPolicyDenied:
        return "policy_denied";
    case redclaw::service::PrivilegedError::kStepUpRequired:
        return "step_up_required";
    case redclaw::service::PrivilegedError::kTokenExpired:
        return "token_expired";
    case redclaw::service::PrivilegedError::kReplayDetected:
        return "replay_detected";
    case redclaw::service::PrivilegedError::kSecureDesktopUnavailable:
        return "secure_desktop_unavailable";
    case redclaw::service::PrivilegedError::kRateLimited:
        return "rate_limited";
    case redclaw::service::PrivilegedError::kInternalError:
        return "internal_error";
    }

    return "unknown";
}

std::string to_string(redclaw::service::PrivilegedDecision decision) {
    switch (decision) {
    case redclaw::service::PrivilegedDecision::kAllow:
        return "allow";
    case redclaw::service::PrivilegedDecision::kDeny:
        return "deny";
    case redclaw::service::PrivilegedDecision::kTimeout:
        return "timeout";
    case redclaw::service::PrivilegedDecision::kBlocked:
        return "blocked";
    }

    return "unknown";
}

std::string to_string(redclaw::service::CapabilityLevel capability) {
    switch (capability) {
    case redclaw::service::CapabilityLevel::kViewOnly:
        return "view-only";
    case redclaw::service::CapabilityLevel::kStandardControl:
        return "standard-control";
    case redclaw::service::CapabilityLevel::kFullControl:
        return "full-control";
    }

    return "unknown";
}

RunResult run_trigger(const Options& options) {
    RunResult result;

    const std::uint64_t now = now_unix_seconds();

    redclaw::service::InMemoryPrivilegedControlBroker broker(
        [now]() { return now; },
        [expected_proof = options.signed_proof](const redclaw::service::PrivilegedRequest& request) {
            return !expected_proof.empty() && request.step_up_proof.signed_proof == expected_proof;
        },
        [&result](const redclaw::service::PrivilegedAuditEvent& event) {
            result.audits.push_back(event);
        },
        [&result](const redclaw::service::CapabilityChangeEvent& event) {
            result.capability_changes.push_back(event);
        });

    redclaw::service::InMemoryUacPromptLifecycleEventSource lifecycle_source;
    redclaw::service::InMemoryUacConsentActionTransport consent_transport;
    redclaw::service::HostServiceUacPromptRuntimeCoordinator coordinator(broker, lifecycle_source, consent_transport);

    redclaw::service::PrivilegedRequest request;
    request.session_id = options.session_id;
    request.step_up_proof.operator_id = options.operator_id;
    request.step_up_proof.device_fingerprint = options.device_id;
    request.step_up_proof.challenge_id = options.challenge_id;
    request.step_up_proof.signed_proof = options.signed_proof;
    request.step_up_proof.issued_at_unix = now;
    request.reason_code = options.reason_code;

    result.grant_result = broker.requestPrivilegedControl(request);
    if (!result.grant_result.accepted) {
        result.error = "request-full-control rejected: " + to_string(result.grant_result.error);
        result.capability_after = broker.currentCapability(options.session_id);
        return result;
    }

    switch (options.prompt_event) {
    case PromptEventMode::kRaised:
        lifecycle_source.emit_prompt_raised(options.session_id, options.uac_prompt_id);
        break;
    case PromptEventMode::kClosed:
        lifecycle_source.emit_prompt_raised(options.session_id, options.uac_prompt_id);
        lifecycle_source.emit_prompt_closed(options.session_id, options.uac_prompt_id);
        break;
    case PromptEventMode::kTimeout:
        lifecycle_source.emit_prompt_raised(options.session_id, options.uac_prompt_id);
        lifecycle_source.emit_prompt_timeout(options.session_id, options.uac_prompt_id);
        break;
    case PromptEventMode::kUnavailable:
        lifecycle_source.emit_prompt_raised(options.session_id, options.uac_prompt_id);
        lifecycle_source.emit_prompt_unavailable(options.session_id, options.uac_prompt_id, options.unavailable_reason);
        break;
    }

    result.prompt_event_applied = coordinator.last_prompt_event_applied();
    result.capability_after = broker.currentCapability(options.session_id);

    if (options.prompt_event == PromptEventMode::kRaised) {
        result.ok = result.prompt_event_applied && result.capability_after == redclaw::service::CapabilityLevel::kFullControl;
    } else {
        result.ok = result.prompt_event_applied && result.capability_after == redclaw::service::CapabilityLevel::kStandardControl;
    }

    if (!result.ok && result.error.empty()) {
        result.error = "prompt trigger did not satisfy expected state transition";
    }

    return result;
}

std::string build_json(const Options& options, const RunResult& result) {
    std::string out;
    out += "{\n";
    out += "  \"ok\": ";
    out += result.ok ? "true" : "false";
    out += ",\n";
    out += "  \"prompt_event\": \"" + escape_json(options.prompt_event_name) + "\",\n";
    out += "  \"session_id\": \"" + escape_json(options.session_id) + "\",\n";
    out += "  \"operator_id\": \"" + escape_json(options.operator_id) + "\",\n";
    out += "  \"device_id\": \"" + escape_json(options.device_id) + "\",\n";
    out += "  \"uac_prompt_id\": \"" + escape_json(options.uac_prompt_id) + "\",\n";
    out += "  \"correlation\": {\n";
    out += "    \"session_id\": \"" + escape_json(options.session_id) + "\",\n";
    out += "    \"token_id\": \"" + escape_json(result.grant_result.grant_token.token_id) + "\",\n";
    out += "    \"uac_prompt_id\": \"" + escape_json(options.uac_prompt_id) + "\"\n";
    out += "  },\n";
    out += "  \"grant\": {\n";
    out += "    \"accepted\": ";
    out += result.grant_result.accepted ? "true" : "false";
    out += ",\n";
    out += "    \"error\": \"" + escape_json(to_string(result.grant_result.error)) + "\",\n";
    out += "    \"token_id\": \"" + escape_json(result.grant_result.grant_token.token_id) + "\",\n";
    out += "    \"expires_at_unix\": " + std::to_string(result.grant_result.grant_token.expires_at_unix) + "\n";
    out += "  },\n";
    out += "  \"prompt_event_applied\": ";
    out += result.prompt_event_applied ? "true" : "false";
    out += ",\n";
    out += "  \"capability_after\": \"" + escape_json(to_string(result.capability_after)) + "\",\n";

    out += "  \"audits\": [\n";
    for (std::size_t i = 0; i < result.audits.size(); ++i) {
        const auto& audit = result.audits[i];
        out += "    {\"session_id\":\"" + escape_json(audit.session_id)
            + "\",\"operator_id\":\"" + escape_json(audit.operator_id)
            + "\",\"action\":\"" + escape_json(audit.action)
            + "\",\"detail\":\"" + escape_json(audit.detail)
            + "\",\"decision\":\"" + escape_json(to_string(audit.decision))
            + "\",\"error\":\"" + escape_json(to_string(audit.error))
            + "\",\"timestamp_unix\":" + std::to_string(audit.timestamp_unix)
            + "}";
        out += (i + 1 < result.audits.size()) ? ",\n" : "\n";
    }
    out += "  ],\n";

    out += "  \"capability_changes\": [\n";
    for (std::size_t i = 0; i < result.capability_changes.size(); ++i) {
        const auto& change = result.capability_changes[i];
        out += "    {\"session_id\":\"" + escape_json(change.session_id)
            + "\",\"before\":\"" + escape_json(to_string(change.before))
            + "\",\"after\":\"" + escape_json(to_string(change.after))
            + "\",\"reason\":\"" + escape_json(change.reason)
            + "\",\"timestamp_unix\":" + std::to_string(change.timestamp_unix)
            + "}";
        out += (i + 1 < result.capability_changes.size()) ? ",\n" : "\n";
    }
    out += "  ],\n";

    out += "  \"error\": \"" + escape_json(result.error) + "\"\n";
    out += "}\n";

    return out;
}

bool write_output_file(std::string_view path, std::string_view content, std::string* error) {
    std::ofstream out_file(std::string(path), std::ios::out | std::ios::trunc);
    if (!out_file.is_open()) {
        *error = "failed to write output file: " + std::string(path);
        return false;
    }

    out_file << content;
    if (!out_file.good()) {
        *error = "failed to flush output file: " + std::string(path);
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
        std::cerr << "[ERROR] " << parse_error << '\n';
        print_usage();
        return 2;
    }

    if (options.help_requested) {
        print_usage();
        return 0;
    }

    RunResult run_result = run_trigger(options);
    const std::string json_output = build_json(options, run_result);
    std::cout << json_output;

    if (!options.output_json_path.empty()) {
        std::string file_error;
        if (!write_output_file(options.output_json_path, json_output, &file_error)) {
            std::cerr << "[ERROR] " << file_error << '\n';
            return 3;
        }
    }

    return run_result.ok ? 0 : 1;
}
