#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "redclaw/service/service_module.h"

namespace {

enum class HarnessAction {
    kInvalid,
    kRequestFullControl,
    kUacAllow,
    kUacDeny,
    kUacTimeout,
};

struct HarnessOptions {
    HarnessAction action = HarnessAction::kInvalid;
    std::string action_name;
    std::string session_id = "m07-harness-session";
    std::string operator_id = "m07-harness-operator";
    std::string device_id = "m07-harness-device";
    std::string challenge_id = "m07-harness-challenge";
    std::string signed_proof = "m07-harness-proof";
    std::uint64_t issued_at_unix = 0;
    std::string token_id;
    std::string uac_prompt_id = "m07-harness-prompt";
    std::string reason_code = "m07_harness";
    std::string output_json_path;
    bool help_requested = false;
};

struct HarnessRunResult {
    bool ok = false;
    std::string error;
    redclaw::service::PrivilegedRequestResult request_result;
    bool prompt_event_applied = false;
    redclaw::service::UacConsentResult consent_result;
    redclaw::service::CapabilityLevel capability_after = redclaw::service::CapabilityLevel::kStandardControl;
    std::vector<redclaw::service::PrivilegedAuditEvent> audits;
    std::vector<redclaw::service::CapabilityChangeEvent> capability_changes;
};

void print_usage() {
    std::cout
        << "Usage: redclaw_m07_controller_harness_cli --action <request-full-control|uac-allow|uac-deny|uac-timeout> [options]\n"
        << "Options:\n"
        << "  --session-id <id>         Session id (default: m07-harness-session)\n"
        << "  --operator-id <id>        Operator id (default: m07-harness-operator)\n"
        << "  --device-id <id>          Device id (default: m07-harness-device)\n"
        << "  --challenge-id <id>       Challenge id for step-up proof\n"
        << "  --signed-proof <value>    Signed proof payload used by step-up verifier\n"
        << "  --issued-at-unix <sec>    Step-up proof issue time (unix seconds; default: now)\n"
        << "  --token-id <id>           Override consent token id (optional)\n"
        << "  --uac-prompt-id <id>      UAC prompt id (default: m07-harness-prompt)\n"
        << "  --reason-code <value>     Reason code for request context\n"
        << "  --output-json-path <path> Write JSON output to file\n"
        << "  --help                    Show this help\n";
}

bool read_value_arg(const std::vector<std::string>& args, std::size_t& index, std::string* out) {
    if (index + 1 >= args.size()) {
        return false;
    }
    ++index;
    *out = args[index];
    return true;
}

HarnessAction parse_action(std::string_view action) {
    if (action == "request-full-control") {
        return HarnessAction::kRequestFullControl;
    }
    if (action == "uac-allow") {
        return HarnessAction::kUacAllow;
    }
    if (action == "uac-deny") {
        return HarnessAction::kUacDeny;
    }
    if (action == "uac-timeout") {
        return HarnessAction::kUacTimeout;
    }
    return HarnessAction::kInvalid;
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
        parsed = (parsed * 10ULL) + static_cast<std::uint64_t>(ch - '0');
    }

    *out = parsed;
    return true;
}

bool parse_options(const std::vector<std::string>& args, HarnessOptions* options, std::string* error) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];

        if (arg == "--help" || arg == "-h") {
            options->help_requested = true;
            return true;
        }

        if (arg == "--action") {
            if (!read_value_arg(args, i, &options->action_name)) {
                *error = "missing value for --action";
                return false;
            }
            options->action = parse_action(options->action_name);
            if (options->action == HarnessAction::kInvalid) {
                *error = "invalid action: " + options->action_name;
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

        if (arg == "--issued-at-unix") {
            std::string issued_at;
            if (!read_value_arg(args, i, &issued_at)) {
                *error = "missing value for --issued-at-unix";
                return false;
            }
            if (!parse_uint64(issued_at, &options->issued_at_unix)) {
                *error = "invalid value for --issued-at-unix: " + issued_at;
                return false;
            }
            continue;
        }

        if (arg == "--token-id") {
            if (!read_value_arg(args, i, &options->token_id)) {
                *error = "missing value for --token-id";
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

        if (arg == "--reason-code") {
            if (!read_value_arg(args, i, &options->reason_code)) {
                *error = "missing value for --reason-code";
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

    if (options->action == HarnessAction::kInvalid && !options->help_requested) {
        *error = "--action is required";
        return false;
    }

    if (options->session_id.empty()) {
        *error = "--session-id cannot be empty";
        return false;
    }

    if (options->operator_id.empty()) {
        *error = "--operator-id cannot be empty";
        return false;
    }

    if (options->device_id.empty()) {
        *error = "--device-id cannot be empty";
        return false;
    }

    return true;
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

bool action_is_uac(HarnessAction action) {
    return action == HarnessAction::kUacAllow
        || action == HarnessAction::kUacDeny
        || action == HarnessAction::kUacTimeout;
}

std::uint64_t now_unix_seconds() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

HarnessRunResult run_harness(const HarnessOptions& options) {
    HarnessRunResult result;
    const std::uint64_t now = now_unix_seconds();

    const std::uint64_t issued_at_unix = options.issued_at_unix == 0 ? now : options.issued_at_unix;

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

    redclaw::service::PrivilegedRequest request;
    request.session_id = options.session_id;
    request.step_up_proof.operator_id = options.operator_id;
    request.step_up_proof.device_fingerprint = options.device_id;
    request.step_up_proof.challenge_id = options.challenge_id;
    request.step_up_proof.signed_proof = options.signed_proof;
    request.step_up_proof.issued_at_unix = issued_at_unix;
    request.reason_code = options.reason_code;

    result.request_result = broker.requestPrivilegedControl(request);
    if (!result.request_result.accepted) {
        result.error = "request-full-control rejected: " + to_string(result.request_result.error);
        result.capability_after = broker.currentCapability(options.session_id);
        return result;
    }

    if (!action_is_uac(options.action)) {
        result.ok = true;
        result.capability_after = broker.currentCapability(options.session_id);
        return result;
    }

    redclaw::service::InMemoryUacPromptLifecycleEventSource lifecycle_source;
    redclaw::service::InMemoryUacConsentActionTransport consent_transport;
    redclaw::service::HostServiceUacPromptRuntimeCoordinator coordinator(broker, lifecycle_source, consent_transport);
    redclaw::service::HostServiceUacConsentSignalProducer consent_producer(consent_transport, options.session_id);

    lifecycle_source.emit_prompt_raised(options.session_id, options.uac_prompt_id);
    result.prompt_event_applied = coordinator.last_prompt_event_applied();
    if (!result.prompt_event_applied) {
        result.error = "beginUacPrompt was not applied";
        result.capability_after = broker.currentCapability(options.session_id);
        return result;
    }

    const std::string action_token = options.token_id.empty() ? result.request_result.grant_token.token_id : options.token_id;

    switch (options.action) {
    case HarnessAction::kUacAllow:
        result.consent_result = consent_producer.submit_allow(action_token, options.uac_prompt_id);
        break;
    case HarnessAction::kUacDeny:
        result.consent_result = consent_producer.submit_deny(action_token, options.uac_prompt_id);
        break;
    case HarnessAction::kUacTimeout:
        // Mirror the fail-close order used by runtime coordinator tests.
        lifecycle_source.emit_prompt_timeout(options.session_id, options.uac_prompt_id);
        result.prompt_event_applied = coordinator.last_prompt_event_applied();
        result.consent_result = consent_producer.submit_timeout(action_token, options.uac_prompt_id);
        break;
    case HarnessAction::kInvalid:
    case HarnessAction::kRequestFullControl:
        break;
    }

    result.capability_after = broker.currentCapability(options.session_id);

    if (options.action == HarnessAction::kUacAllow) {
        result.ok = result.consent_result.applied && result.consent_result.error == redclaw::service::PrivilegedError::kNone;
    } else if (options.action == HarnessAction::kUacDeny) {
        result.ok = result.consent_result.applied
            && result.consent_result.final_decision == redclaw::service::PrivilegedDecision::kDeny
            && result.consent_result.error == redclaw::service::PrivilegedError::kNone;
    } else if (options.action == HarnessAction::kUacTimeout) {
        result.ok = !result.consent_result.applied
            && result.consent_result.error == redclaw::service::PrivilegedError::kPolicyDenied
            && result.capability_after == redclaw::service::CapabilityLevel::kStandardControl;
    }

    if (!result.ok && result.error.empty()) {
        result.error = "consent action did not satisfy expected outcome";
    }

    return result;
}

std::string build_json(const HarnessOptions& options, const HarnessRunResult& result) {
    std::string out;
    out += "{\n";
    out += "  \"action\": \"" + escape_json(options.action_name) + "\",\n";
    out += "  \"ok\": ";
    out += result.ok ? "true" : "false";
    out += ",\n";
    out += "  \"session_id\": \"" + escape_json(options.session_id) + "\",\n";
    out += "  \"operator_id\": \"" + escape_json(options.operator_id) + "\",\n";
    out += "  \"device_id\": \"" + escape_json(options.device_id) + "\",\n";
    out += "  \"uac_prompt_id\": \"" + escape_json(options.uac_prompt_id) + "\",\n";
    out += "  \"request\": {\n";
    out += "    \"accepted\": ";
    out += result.request_result.accepted ? "true" : "false";
    out += ",\n";
    out += "    \"error\": \"" + escape_json(to_string(result.request_result.error)) + "\",\n";
    out += "    \"granted_level\": \"" + escape_json(to_string(result.request_result.granted_level)) + "\",\n";
    out += "    \"token_id\": \"" + escape_json(result.request_result.grant_token.token_id) + "\",\n";
    out += "    \"expires_at_unix\": " + std::to_string(result.request_result.grant_token.expires_at_unix) + "\n";
    out += "  },\n";
    out += "  \"prompt\": {\n";
    out += "    \"event_applied\": ";
    out += result.prompt_event_applied ? "true" : "false";
    out += "\n";
    out += "  },\n";
    out += "  \"consent\": {\n";
    out += "    \"applied\": ";
    out += result.consent_result.applied ? "true" : "false";
    out += ",\n";
    out += "    \"error\": \"" + escape_json(to_string(result.consent_result.error)) + "\",\n";
    out += "    \"final_decision\": \"" + escape_json(to_string(result.consent_result.final_decision)) + "\"\n";
    out += "  },\n";
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

    HarnessOptions options;
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

    HarnessRunResult run_result = run_harness(options);
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
