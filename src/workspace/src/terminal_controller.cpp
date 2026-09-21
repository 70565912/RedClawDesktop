#include "redclaw/workspace/terminal_controller.h"
#include "terminal_identity.h"
#include <algorithm>

namespace redclaw::workspace {
using protocol::TerminalMessageTypeV1;
TerminalController::TerminalController(std::function<bool(const Message&)> send, Surface surface)
    : send_(std::move(send)), surface_(std::move(surface)), client_session_id_(new_terminal_identity()) {}
TerminalController::Message TerminalController::command(TerminalMessageTypeV1 type) const {
    Message message;
    message.type = type; message.session_epoch = epoch_; message.terminal_id = id_;
    message.input_generation = input_generation_;
    message.client_session_id = client_session_id_;
    return message;
}
void TerminalController::discard_input() { pending_input_.clear(); input_in_flight_ = 0; }
void TerminalController::show_state(std::string_view error) {
    if (surface_.state) surface_.state(input_enabled(), error);
}
void TerminalController::request_open() {
    if (ending_) {
        if (!end_complete()) return;
        client_session_id_ = new_terminal_identity(); id_.clear();
        ending_ = end_sent_ = end_acknowledged_ = end_wait_expired_ = end_required_ = opened_ = enabled_ = false;
    }
    requested_ = true;
    if (opened_ && !enabled_ && !id_.empty()) { opened_ = false; discard_input(); }
    pump();
}
void TerminalController::request_end() {
    end_required_ = end_required_ || opened_ || !id_.empty();
    ending_ = true; requested_ = false; discard_input(); show_state(); pump();
}
bool TerminalController::end_complete() const { return ending_ && (!end_required_ || !available_ || end_acknowledged_ || end_wait_expired_); }
bool TerminalController::end_acknowledged() const { return end_acknowledged_ || !end_required_; }
void TerminalController::expire_end_wait() { end_wait_expired_ = true; }
void TerminalController::set_surface_input_paused(bool paused) {
    surface_input_paused_ = paused;
    if (paused) discard_input();
    show_state(); pump();
}
void TerminalController::disconnected() {
    if (!execution_id_.empty()) {
        auto message = command(TerminalMessageTypeV1::kExecState); message.operation_id = execution_id_;
        message.execution_state = "unknown"; message.error_code = "terminal_connection_lost";
        if (surface_.execution) surface_.execution(message);
        execution_id_.clear();
    }
    prompt_ready_ = false;
    available_ = enabled_ = opened_ = false; discard_input(); show_state("terminal_connection_unavailable");
}
bool TerminalController::receive(const Message& message) {
    if (!protocol::validate_terminal_message_v1(message)) return false;
    if (message.type == TerminalMessageTypeV1::kAvailability) {
        if (!message.input_enabled || (!epoch_.empty() && epoch_ != message.session_epoch)) disconnected();
        capability_version_ = message.capability_version;
        if (epoch_ != message.session_epoch || available_ != message.input_enabled) {
            enabled_ = opened_ = false; discard_input();
        }
        epoch_ = message.session_epoch; available_ = message.input_enabled;
        local_input_paused_ = message.local_input_paused;
        if (local_input_paused_) discard_input();
        if (!available_) { enabled_ = opened_ = false; discard_input(); }
        show_state(message.error_code); pump(); return true;
    }
    if (!available_ || message.session_epoch != epoch_) return false;
    if (message.type == TerminalMessageTypeV1::kExecState) {
        if (id_ != message.terminal_id || message.operation_id != execution_id_) return false;
        if (surface_.execution) surface_.execution(message);
        if (message.execution_state != "running" && message.execution_state != "output"
            && message.execution_state != "cancelling") execution_id_.clear();
        if (message.execution_state == "rejected") {
            input_in_flight_ = 0; input_sequence_ = message.input_sequence;
            input_generation_ = message.input_generation;
        }
        show_state(); return true;
    }
    if (message.type == TerminalMessageTypeV1::kEnded) {
        if (!ending_ || message.client_session_id != client_session_id_
            || (!id_.empty() && message.terminal_id != id_)) return false;
        end_acknowledged_ = true; enabled_ = opened_ = false;
        pending_output_.reset(); output_submitted_ = ack_pending_ = false; show_state(); return true;
    }
    if (message.type == TerminalMessageTypeV1::kReady || message.type == TerminalMessageTypeV1::kState) {
        if (!opened_) return false;
        if (id_ != message.terminal_id) {
            // State is also the retry for a Ready that could not be sent.
            id_ = message.terminal_id; parsed_sequence_ = message.sequence;
            pending_output_.reset(); output_submitted_ = ack_pending_ = false;
            ready_ = false;
            if (surface_.reset) surface_.reset(id_);
        }
        if (input_generation_ != message.input_generation || !message.input_enabled) discard_input();
        input_generation_ = message.input_generation;
        if (!input_in_flight_) input_sequence_ = message.input_sequence;
        else if (message.input_sequence >= input_in_flight_) { input_in_flight_ = 0; input_sequence_ = message.input_sequence; }
        enabled_ = message.input_enabled && !message.exited && message.error_code.empty();
        prompt_ready_ = message.prompt_ready;
        if (!enabled_) discard_input();
        if (message.type == TerminalMessageTypeV1::kReady) resize_pending_ = true;
        show_state(message.exited && message.error_code.empty() ? "terminal_process_exited" : message.error_code);
        pump(); return true;
    }
    if (message.type != TerminalMessageTypeV1::kOutput || id_ != message.terminal_id) return false;
    if (message.sequence <= parsed_sequence_) { ack_pending_ = true; pump(); return true; }
    if (message.sequence != parsed_sequence_ + 1) return false;
    if (pending_output_) return pending_output_->sequence == message.sequence && pending_output_->bytes == message.bytes;
    pending_output_ = message; pump(); return true;
}
bool TerminalController::input(std::string_view bytes) {
    if (!input_enabled() || bytes.empty()) return false;
    if (!execution_id_.empty()) return bytes == "\x03" && cancel_execution();
    prompt_ready_ = false;
    if (pending_input_.size() + bytes.size() > 64U * 1024U) {
        show_state("terminal_input_backpressure"); return false;
    }
    pending_input_.append(bytes); pump(); return true;
}
bool TerminalController::execute(std::string id, std::string_view bytes) {
    if (capability_version_ < 2 || !input_enabled() || !prompt_ready() || bytes.empty()
        || bytes.size() > protocol::kMaxTerminalChunkBytes) return false;
    auto message = command(TerminalMessageTypeV1::kExec); message.operation_id = id;
    message.sequence = input_sequence_ + 1; message.bytes = bytes;
    if (!protocol::validate_terminal_message_v1(message) || !send_ || !send_(message)) return false;
    execution_id_ = std::move(id); prompt_ready_ = false;
    input_sequence_ = input_in_flight_ = message.sequence; show_state(); return true;
}
bool TerminalController::cancel_execution() {
    if (execution_id_.empty() || !available_ || !send_) return false;
    auto message = command(TerminalMessageTypeV1::kCancel); message.operation_id = execution_id_;
    return send_(message);
}
void TerminalController::resize(std::uint32_t columns, std::uint32_t rows) {
    if (columns < 2 || columns > 32767 || rows < 2 || rows > 32767) return;
    columns_ = columns; rows_ = rows; resize_pending_ = true; pump();
}
void TerminalController::surface_ready(bool ready) { ready_ = ready; show_state(); pump(); }
void TerminalController::output_parsed() {
    if (pending_output_ && output_submitted_) {
        parsed_sequence_ = pending_output_->sequence;
        pending_output_.reset(); output_submitted_ = false; ack_pending_ = true;
    }
    pump();
}
void TerminalController::pump() {
    if (!available_ || !send_) return;
    if (ending_) {
        if (end_required_ && !end_sent_) end_sent_ = send_(command(TerminalMessageTypeV1::kEnd));
        return;
    }
    if (requested_ && ready_ && !opened_ && !local_input_paused_ && !surface_input_paused_) {
        auto message = command(TerminalMessageTypeV1::kOpen);
        message.terminal_id.clear(); message.columns = columns_; message.rows = rows_;
        if (!send_(message)) return;
        opened_ = true;
    }
    if (!id_.empty() && ack_pending_) {
        auto message = command(TerminalMessageTypeV1::kOutputAck); message.sequence = parsed_sequence_;
        if (!send_(message)) return;
        ack_pending_ = false;
    }
    if (ready_ && pending_output_ && !output_submitted_ && surface_.output) {
        output_submitted_ = surface_.output(pending_output_->bytes);
    }
    if (!input_enabled()) return;
    if (!input_in_flight_ && !pending_input_.empty()) {
        auto message = command(TerminalMessageTypeV1::kInput);
        message.sequence = input_sequence_ + 1;
        message.bytes = pending_input_.substr(0, protocol::kMaxTerminalChunkBytes);
        if (!send_(message)) return;
        pending_input_.erase(0, message.bytes.size());
        input_in_flight_ = message.sequence; input_sequence_ = message.sequence;
    }
    if (resize_pending_ && execution_id_.empty()) {
        auto message = command(TerminalMessageTypeV1::kResize); message.columns = columns_; message.rows = rows_;
        if (send_(message)) resize_pending_ = false;
    }
}
bool TerminalController::input_enabled() const {
    return available_ && enabled_ && ready_ && opened_ && !id_.empty() && !ending_ && !local_input_paused_ && !surface_input_paused_;
}
std::size_t TerminalController::pending_input_bytes() const { return pending_input_.size(); }
std::string_view TerminalController::terminal_id() const { return id_; }
}
