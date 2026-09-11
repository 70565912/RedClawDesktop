#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>

#include "redclaw/protocol/protocol_module.h"
#include "redclaw/session/session_module.h"

namespace {

bool parse_u32_arg(const std::string& value, std::uint32_t* output) {
    if (output == nullptr || value.empty()) {
        return false;
    }

    try {
        const unsigned long parsed = std::stoul(value);
        if (parsed > 0xFFFFFFFFUL) {
            return false;
        }
        *output = static_cast<std::uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

std::string build_json_report(
    bool ok,
    std::uint32_t iterations,
    std::uint32_t completed_iterations,
    std::uint32_t recoveries,
    std::uint32_t throttled_failures,
    std::uint32_t terminated_iterations,
    std::uint64_t elapsed_ms,
    const std::string& error) {
    std::string json;
    json += "{\n";
    json += "  \"ok\": ";
    json += ok ? "true" : "false";
    json += ",\n";
    json += "  \"iterations\": " + std::to_string(iterations) + ",\n";
    json += "  \"completedIterations\": " + std::to_string(completed_iterations) + ",\n";
    json += "  \"recoveries\": " + std::to_string(recoveries) + ",\n";
    json += "  \"throttledFailures\": " + std::to_string(throttled_failures) + ",\n";
    json += "  \"terminatedIterations\": " + std::to_string(terminated_iterations) + ",\n";
    json += "  \"elapsedMs\": " + std::to_string(elapsed_ms) + ",\n";
    json += "  \"error\": \"" + error + "\"\n";
    json += "}\n";
    return json;
}

void print_usage() {
    std::cout
        << "Usage: redclaw_session_recovery_chaos_runner "
        << "[--iterations N] [--retry-backoff-ms N] [--report-file PATH]" << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    std::uint32_t iterations = 500;
    std::uint32_t retry_backoff_ms = 75;
    std::string report_file_path;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto read_next_u32 = [&](std::uint32_t* target) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            ++i;
            return parse_u32_arg(argv[i], target);
        };

        if (arg == "--iterations") {
            if (!read_next_u32(&iterations) || iterations == 0) {
                std::cerr << "Invalid --iterations" << '\n';
                return 2;
            }
        } else if (arg == "--retry-backoff-ms") {
            if (!read_next_u32(&retry_backoff_ms)) {
                std::cerr << "Invalid --retry-backoff-ms" << '\n';
                return 2;
            }
        } else if (arg == "--report-file") {
            if (i + 1 >= argc) {
                std::cerr << "Invalid --report-file" << '\n';
                return 2;
            }
            ++i;
            report_file_path = argv[i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            print_usage();
            return 2;
        }
    }

    redclaw::session::SessionRecoveryPolicy policy;
    policy.recovery_timeout_ms = 8000;
    policy.max_retry_count = 4;
    policy.retry_backoff_ms = retry_backoff_ms;

    std::uint64_t fake_now = 1000;
    redclaw::session::SessionRecoveryOrchestrator orchestrator(
        policy,
        redclaw::protocol::SessionStateV1::established,
        [&fake_now]() { return fake_now; });

    std::uint32_t completed_iterations = 0;
    std::uint32_t recoveries = 0;
    std::uint32_t throttled_failures = 0;
    std::uint32_t terminated_iterations = 0;
    std::string error;

    for (std::uint32_t i = 0; i < iterations; ++i) {
        const auto disconnected = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kDisconnected);
        if (!disconnected.transitioned || disconnected.state != redclaw::protocol::SessionStateV1::recovering) {
            error = "disconnect did not enter recovering state";
            break;
        }

        const std::uint32_t fail_count = i % 3;
        for (std::uint32_t j = 0; j < fail_count; ++j) {
            const auto immediate_fail = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kFailed);
            if (!immediate_fail.error.empty() && immediate_fail.error == "retry backoff window active") {
                ++throttled_failures;
            }

            fake_now += policy.retry_backoff_ms;
            const auto budgeted_fail = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kFailed);
            if (budgeted_fail.transitioned && budgeted_fail.state == redclaw::protocol::SessionStateV1::terminated) {
                ++terminated_iterations;
                error = "unexpected termination while still within chaos budget";
                break;
            }
        }

        if (!error.empty()) {
            break;
        }

        fake_now += policy.retry_backoff_ms;
        const auto recovered = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kConnected);
        if (!recovered.transitioned || recovered.state != redclaw::protocol::SessionStateV1::established) {
            error = "recovery did not return to established state";
            break;
        }

        ++recoveries;
        ++completed_iterations;

        const auto noise_connected = orchestrator.on_connectivity_signal(redclaw::session::ConnectivityHealthSignal::kConnected);
        if (noise_connected.transitioned || noise_connected.state != redclaw::protocol::SessionStateV1::established) {
            error = "spurious connected signal changed established state";
            break;
        }
    }

    const bool ok = error.empty() && terminated_iterations == 0;
    const std::uint64_t elapsed_ms = fake_now - 1000;
    const std::string report = build_json_report(
        ok,
        iterations,
        completed_iterations,
        recoveries,
        throttled_failures,
        terminated_iterations,
        elapsed_ms,
        error);

    std::cout << report;

    if (!report_file_path.empty()) {
        const std::filesystem::path report_path(report_file_path);
        const auto parent = report_path.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                std::cerr << "Failed to create report directory: " << parent.string() << '\n';
                return 3;
            }
        }

        std::ofstream out(report_file_path, std::ios::binary);
        if (!out.is_open()) {
            std::cerr << "Failed to open report file: " << report_file_path << '\n';
            return 3;
        }

        out << report;
        if (!out.good()) {
            std::cerr << "Failed to write report file: " << report_file_path << '\n';
            return 3;
        }
    }

    return ok ? 0 : 1;
}
