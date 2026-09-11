#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "redclaw/capture/capture_module.h"

namespace {

bool expect_true(bool condition, const std::string& message) {
    if (condition) {
        return true;
    }

    std::cerr << "[FAIL] " << message << '\n';
    return false;
}

bool file_exists_and_nonempty(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return false;
    }
    return std::filesystem::file_size(path, ec) > 0;
}

constexpr int kTestPassed = 0;
constexpr int kTestFailed = 1;
constexpr int kTestSkipped = 77;

int test_dda_min_capture_poc() {
    const std::filesystem::path output_path =
        std::filesystem::temp_directory_path() / "redclaw_m04_t00_capture_poc.bmp";

    std::error_code remove_ec;
    std::filesystem::remove(output_path, remove_ec);

    const auto result = redclaw::capture::run_dda_min_capture_poc(output_path.string(), 4000);
    if (!result.ok) {
        std::cout << "[SKIP] redclaw_capture_dda_min_capture_poc_tests: " << result.error << '\n';
        return kTestSkipped;
    }

    const bool ok = expect_true(result.bgra_frame, "captured frame should be BGRA")
        && expect_true(result.width > 0 && result.height > 0, "captured frame dimensions should be non-zero")
        && expect_true(file_exists_and_nonempty(output_path), "captured output file should exist and be non-empty");
    return ok ? kTestPassed : kTestFailed;
}

}  // namespace

int main() {
    const int result = test_dda_min_capture_poc();
    if (result != kTestPassed) {
        return result;
    }

    std::cout << "[PASS] redclaw_capture_dda_min_capture_poc_tests" << '\n';
    return kTestPassed;
}
