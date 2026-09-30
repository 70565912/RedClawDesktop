// Developer-invoked timing probe, deliberately not a CTest performance gate.
#include "../../src/net/src/media_pacer_wait.h"

#include <array>
#include <ctime>
#include <iostream>
#include <string_view>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
double cpu_us() {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return -1;
    const auto ticks = [](FILETIME time) {
        return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) / 10.0;
#else
    return 1000000.0 * static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
#endif
}
}

int main(int argc, char** argv) {
    using Wait = redclaw::net::MediaPacerWait;
    if (argc != 2 || (std::string_view(argv[1]) != "automatic"
        && std::string_view(argv[1]) != "condition-variable")) {
        std::cerr << "Usage: redclaw_media_pacer_wait_probe automatic|condition-variable\n";
        return 1;
    }
    const bool automatic = std::string_view(argv[1]) == "automatic";
    Wait wait(automatic ? Wait::Mode::kAutomatic : Wait::Mode::kConditionVariable);
    constexpr std::array requests{100, 250, 500, 1000, 2000, 4000, 8000, 16000};
    std::mutex mutex;
    std::unique_lock lock(mutex);
    for (unsigned i = 0; i < 32; ++i)
        wait.wait_for(lock, std::chrono::microseconds(requests[i % requests.size()]), [] { return false; });
    std::vector<long long> elapsed;
    elapsed.reserve(512);
    const auto cpu_begin = cpu_us();
    const auto wall_begin = Wait::Clock::now();
    for (unsigned i = 0; i < 512; ++i) {
        const auto begin = Wait::Clock::now();
        wait.wait_for(lock, std::chrono::microseconds(requests[i % requests.size()]), [] { return false; });
        elapsed.push_back(std::chrono::duration_cast<std::chrono::microseconds>(Wait::Clock::now() - begin).count());
    }
    const auto wall_us = std::chrono::duration_cast<std::chrono::microseconds>(Wait::Clock::now() - wall_begin).count();
    const auto used_cpu_us = cpu_us() - cpu_begin;
    std::cout << "# high_resolution=" << wait.high_resolution() << " samples=" << elapsed.size()
              << " wall_us=" << wall_us << " cpu_us=" << used_cpu_us << '\n';
    std::cout << "requested_us,elapsed_us\n";
    for (std::size_t i = 0; i < elapsed.size(); ++i)
        std::cout << requests[i % requests.size()] << ',' << elapsed[i] << '\n';
    return 0;
}
