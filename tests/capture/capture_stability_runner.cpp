#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>

#include "redclaw/capture/capture_module.h"

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

std::string backend_to_string(redclaw::capture::CaptureBackendType backend) {
    switch (backend) {
    case redclaw::capture::CaptureBackendType::kDesktopDuplication:
        return "DesktopDuplication";
    case redclaw::capture::CaptureBackendType::kWindowsGraphicsCapture:
        return "WindowsGraphicsCapture";
    case redclaw::capture::CaptureBackendType::kGdiBitBlt:
        return "GdiBitBlt";
    default:
        return "Unknown";
    }
}

std::string build_json_report(const redclaw::capture::CaptureStabilityRunResult& result) {
    std::string json;
    json += "{\n";
    json += "  \"ok\": ";
    json += result.ok ? "true" : "false";
    json += ",\n";
    json += "  \"error\": \"" + result.error + "\",\n";
    json += "  \"backend\": \"" + backend_to_string(result.backend) + "\",\n";
    json += "  \"backendSwitchCount\": " + std::to_string(result.backend_switch_count) + ",\n";
    json += "  \"fallbackAttemptCount\": " + std::to_string(result.fallback_attempt_count) + ",\n";
    json += "  \"framesCaptured\": " + std::to_string(result.frames_captured) + ",\n";
    json += "  \"captureAttempts\": " + std::to_string(result.capture_attempts) + ",\n";
    json += "  \"timeoutCount\": " + std::to_string(result.timeout_count) + ",\n";
    json += "  \"failureCount\": " + std::to_string(result.failure_count) + ",\n";
    json += "  \"maxConsecutiveTimeoutsSeen\": " + std::to_string(result.max_consecutive_timeouts_seen) + ",\n";
    json += "  \"maxConsecutiveFailuresSeen\": " + std::to_string(result.max_consecutive_failures_seen) + ",\n";
    json += "  \"staleFrameCount\": " + std::to_string(result.stale_frame_count) + ",\n";
    json += "  \"blackFrameRatio\": " + std::to_string(result.black_frame_ratio) + ",\n";
    json += "  \"framePresentDeltaMs\": " + std::to_string(result.frame_present_delta_ms) + ",\n";
    json += "  \"averageFps\": " + std::to_string(result.average_fps) + ",\n";
    json += "  \"runDurationMs\": " + std::to_string(result.run_duration_ms) + "\n";
    json += "}\n";
    return json;
}

void print_usage() {
    std::cout
        << "Usage: redclaw_capture_stability_runner "
        << "[--duration-seconds N] [--output-index N] [--frame-timeout-ms N] "
        << "[--max-consecutive-timeouts N] [--max-consecutive-failures N] [--report-file PATH]" << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    redclaw::capture::CaptureStabilityRunConfig config;
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

        if (arg == "--duration-seconds") {
            if (!read_next_u32(&config.run_duration_seconds)) {
                std::cerr << "Invalid --duration-seconds" << '\n';
                return 2;
            }
        } else if (arg == "--output-index") {
            if (!read_next_u32(&config.capture_config.output_index)) {
                std::cerr << "Invalid --output-index" << '\n';
                return 2;
            }
        } else if (arg == "--frame-timeout-ms") {
            if (!read_next_u32(&config.capture_config.frame_acquire_timeout_ms)) {
                std::cerr << "Invalid --frame-timeout-ms" << '\n';
                return 2;
            }
        } else if (arg == "--max-consecutive-timeouts") {
            if (!read_next_u32(&config.max_consecutive_timeouts)) {
                std::cerr << "Invalid --max-consecutive-timeouts" << '\n';
                return 2;
            }
        } else if (arg == "--max-consecutive-failures") {
            if (!read_next_u32(&config.max_consecutive_failures)) {
                std::cerr << "Invalid --max-consecutive-failures" << '\n';
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

    redclaw::capture::CaptureStabilityRunResult result;
    std::string error;
    const bool ok = redclaw::capture::run_capture_stability_probe(config, &result, &error);

    if (!ok && result.error.empty()) {
        result.error = error;
    }

    const std::string report = build_json_report(result);
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
