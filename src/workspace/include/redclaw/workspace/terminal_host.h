#pragma once
#include "redclaw/protocol/terminal_protocol.h"
#include "redclaw/workspace/terminal_session.h"
#include "redclaw/workspace/terminal_shell_integration.h"
#include <deque>
#include <functional>

namespace redclaw::workspace {

// Owned by one desktop-session scheduler. This adapter preserves a terminal
// across transient channel loss; a new desktop session explicitly ends it.
class TerminalHost final {
public:
    using Sender = std::function<bool(const protocol::TerminalMessageV1&)>;
    explicit TerminalHost(Sender sender);
    void begin_session(std::string epoch, std::filesystem::path working_directory);
    // Rebinding is only for a new transport epoch of this same desktop session.
    void rebind_epoch(std::string epoch);
    void set_connection(bool channel_open, bool input_eligible);
    void set_capability(std::uint32_t version) { capability_ = version; }
    bool receive(const protocol::TerminalMessageV1& message, std::string* error = nullptr);
    void pump();
    void end_session();
    [[nodiscard]] bool running() const;
    [[nodiscard]] std::size_t buffered_output_bytes() const;
    [[nodiscard]] std::string_view terminal_id() const;
private:
    void send_state(protocol::TerminalMessageTypeV1 type);
    void shell_event(const TerminalShellIntegration::Event& event);
    void execution_result(std::string state, std::string error = {});
    Sender sender_;
    TerminalSession terminal_;
    TerminalShellIntegration integration_;
    std::deque<protocol::TerminalMessageV1> execution_events_;
    std::optional<TerminalShellIntegration::Event> completion_;
    std::string execution_id_;
    std::uint32_t capability_ = 1;
    bool prompt_ready_ = false, cancel_requested_ = false;
    std::uint64_t output_position_ = 0;
    std::string epoch_, terminal_id_, error_code_;
    std::filesystem::path working_directory_;
    std::optional<protocol::TerminalMessageV1> pending_output_;
    std::uint64_t consumed_sequence_ = 0, input_sequence_ = 0, input_generation_ = 0;
    bool connected_ = false, input_eligible_ = false, exited_ = false;
    bool output_sent_ = false, pending_was_sent_ = false, state_dirty_ = false;
    bool start_attempted_ = false;
    bool peer_ready_ = false;
    bool ended_ = false, end_ack_pending_ = false;
    std::string client_session_id_;
};
}
