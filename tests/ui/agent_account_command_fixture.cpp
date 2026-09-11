#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    const auto contains = [&](const std::string& value) {
        return std::find(arguments.begin(), arguments.end(), value) != arguments.end();
    };

    if (contains("--version")) {
        std::cout << "agent-account fixture\n";
        return 0;
    }

    const std::string executable = std::filesystem::path(argv[0]).filename().string();
    const bool cursor = executable.find("cursor-agent") != std::string::npos;
    if (cursor) {
        if (contains("status")) {
            std::cout << "Not logged in\n";
            return 1;
        }
        return 1;
    }

    if (contains("login") && contains("status")) {
        std::cout << "Not logged in\n";
        return 1;
    }
    if (contains("login")) {
        std::cout << "Open browser to continue\n" << std::flush;
        char* mode_value = nullptr;
        std::size_t mode_length = 0;
        const bool wait_mode = _dupenv_s(
            &mode_value, &mode_length, "REDCLAW_AGENT_LOGIN_TEST_MODE") == 0
            && mode_value != nullptr && std::string(mode_value) == "wait";
        std::free(mode_value);
        if (wait_mode) {
            std::cout << "Waiting for browser completion\n" << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(30));
            return 0;
        }
        std::cerr << "Browser launch unavailable\n";
        return 17;
    }
    if (contains("app-server")) {
        return 0;
    }
    return 0;
}
