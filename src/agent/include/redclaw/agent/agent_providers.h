#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "redclaw/agent/remote_agent_broker.h"

namespace redclaw::agent {

using AgentProcessLineCallback = std::function<void(std::string)>;
using AgentProcessExitCallback = std::function<void(int)>;

struct AgentResolvedCommand {
    std::filesystem::path application;
    std::vector<std::string> arguments;
    std::filesystem::path target;
};

[[nodiscard]] bool resolve_agent_command(
    const std::string& executable,
    const std::vector<std::string>& arguments,
    AgentResolvedCommand* resolved,
    std::string* error = nullptr);

class IAgentProcess {
public:
    virtual ~IAgentProcess() = default;
    [[nodiscard]] virtual bool executable_available(
        const std::string& executable,
        std::string* version = nullptr) = 0;
    virtual bool run_probe(
        const std::string& executable,
        const std::vector<std::string>& arguments,
        std::uint32_t timeout_ms,
        int* exit_code,
        std::string* output,
        std::string* error = nullptr) = 0;
    virtual bool start(
        const std::string& executable,
        const std::vector<std::string>& arguments,
        const std::filesystem::path& working_directory,
        AgentProcessLineCallback stdout_line,
        AgentProcessLineCallback stderr_line,
        AgentProcessExitCallback exited,
        std::string* error = nullptr) = 0;
    virtual bool write_line(const std::string& line, std::string* error = nullptr) = 0;
    virtual void interrupt() = 0;
    virtual void stop() = 0;
};

[[nodiscard]] std::unique_ptr<IAgentProcess> make_system_agent_process();
[[nodiscard]] std::unique_ptr<IAgentProvider> make_debug_fixture_agent_provider();

[[nodiscard]] std::unique_ptr<IAgentProvider> make_codex_app_server_provider(
    std::unique_ptr<IAgentProcess> process = make_system_agent_process());
[[nodiscard]] std::unique_ptr<IAgentProvider> make_cursor_agent_provider(
    std::unique_ptr<IAgentProcess> process = make_system_agent_process());

}  // namespace redclaw::agent
