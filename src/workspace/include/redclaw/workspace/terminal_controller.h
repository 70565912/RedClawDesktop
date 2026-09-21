#pragma once
#include "redclaw/protocol/terminal_protocol.h"
#include <functional>
#include <optional>

namespace redclaw::workspace {
class TerminalController final {
public:
    using Message = protocol::TerminalMessageV1;
    struct Surface {
        std::function<void(std::string_view)> reset;
        std::function<bool(std::string_view)> output;
        std::function<void(bool, std::string_view)> state;
        std::function<void(const Message&)> execution;
    };
    TerminalController(std::function<bool(const Message&)> send, Surface surface);
    void request_open();
    void request_end();
    [[nodiscard]] bool end_complete() const;
    [[nodiscard]] bool end_acknowledged() const;
    void expire_end_wait();
    void set_surface_input_paused(bool paused);
    void disconnected();
    bool receive(const Message& message);
    bool input(std::string_view bytes);
    bool execute(std::string id, std::string_view bytes);
    bool cancel_execution();
    [[nodiscard]] bool prompt_ready() const { return prompt_ready_ && pending_input_.empty() && !input_in_flight_ && execution_id_.empty(); }
    [[nodiscard]] std::uint32_t capability_version() const { return capability_version_; }
    [[nodiscard]] std::string_view execution_id() const { return execution_id_; }
    void resize(std::uint32_t columns, std::uint32_t rows);
    void surface_ready(bool ready);
    void output_parsed();
    void pump();
    [[nodiscard]] bool input_enabled() const;
    [[nodiscard]] bool input_paused() const { return local_input_paused_ || surface_input_paused_; }
    [[nodiscard]] std::size_t pending_input_bytes() const;
    [[nodiscard]] std::string_view terminal_id() const;
private:
    Message command(protocol::TerminalMessageTypeV1 type) const;
    void discard_input();
    void show_state(std::string_view error = {});
    std::function<bool(const Message&)> send_;
    Surface surface_;
    std::string epoch_, id_, pending_input_;
    std::optional<Message> pending_output_;
    std::uint64_t input_generation_ = 0, input_sequence_ = 0, input_in_flight_ = 0, parsed_sequence_ = 0;
    std::uint32_t columns_ = 100, rows_ = 30;
    bool available_ = false, enabled_ = false, requested_ = false, opened_ = false, ready_ = false;
    bool output_submitted_ = false, ack_pending_ = false, resize_pending_ = false;
    bool local_input_paused_ = false;
    bool surface_input_paused_ = false;
    std::string client_session_id_;
    bool ending_ = false, end_sent_ = false, end_acknowledged_ = false;
    bool end_wait_expired_ = false;
    bool end_required_ = false;
    bool prompt_ready_ = false;
    std::uint32_t capability_version_ = 0;
    std::string execution_id_;
};
}
