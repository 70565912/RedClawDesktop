#include "redclaw/workspace/terminal_host.h"
#include "terminal_identity.h"

namespace redclaw::workspace {
namespace {
using protocol::TerminalMessageTypeV1;
bool reject(std::string* error, const char* reason) { if (error) *error = reason; return false; }
}
TerminalHost::TerminalHost(Sender sender) : sender_(std::move(sender)) {}
void TerminalHost::begin_session(std::string epoch, std::filesystem::path directory) {
    end_session();
    epoch_ = std::move(epoch); working_directory_ = std::move(directory);
}
void TerminalHost::set_connection(bool open, bool eligible) {
    if (connected_ == open && input_eligible_ == eligible) return;
    if (!open || !eligible) terminal_.discard_pending_input();
    if (!open) peer_ready_ = false;
    if (open && eligible && (!connected_ || !input_eligible_)) ++input_generation_;
    if (connected_ != open) output_sent_ = false;
    connected_ = open; input_eligible_ = eligible;
    state_dirty_ = !terminal_id_.empty();
}
void TerminalHost::rebind_epoch(std::string epoch) {
    if (epoch == epoch_) return;
    terminal_.discard_pending_input();
    epoch_ = std::move(epoch);
    if (pending_output_) pending_output_->session_epoch = epoch_;
    ++input_generation_; output_sent_ = false;
    peer_ready_ = false;
    state_dirty_ = !terminal_id_.empty();
}
void TerminalHost::send_state(TerminalMessageTypeV1 type) {
    if (!connected_ || terminal_id_.empty()) return;
    protocol::TerminalMessageV1 state;
    state.type = type; state.session_epoch = epoch_; state.terminal_id = terminal_id_;
    state.client_session_id = client_session_id_;
    state.sequence = consumed_sequence_;
    state.input_generation = input_generation_; state.input_sequence = input_sequence_;
    state.input_enabled = input_eligible_ && terminal_.running();
    state.exited = exited_; state.error_code = error_code_;
    state_dirty_ = !sender_ || !sender_(state);
}
bool TerminalHost::receive(const protocol::TerminalMessageV1& message, std::string* error) {
    if (!protocol::validate_terminal_message_v1(message, error)) return false;
    if (!connected_ || epoch_.empty() || message.session_epoch != epoch_) return reject(error, "terminal_stale_session");
    if (message.type == TerminalMessageTypeV1::kOpen) {
        if (!client_session_id_.empty() && client_session_id_ != message.client_session_id) {
            // A newly opened desktop must not inherit a disconnected client's
            // shell, pending commands or output. Transport reconnect retains its client identity.
            auto epoch = epoch_; auto directory = working_directory_;
            const bool eligible = input_eligible_;
            begin_session(std::move(epoch), std::move(directory)); set_connection(true, eligible);
        }
        client_session_id_ = message.client_session_id;
        if (ended_) { end_ack_pending_ = true; return reject(error, "terminal_session_ended"); }
        if (!message.terminal_id.empty() && message.terminal_id != terminal_id_) return reject(error, "terminal_stale_identity");
        if (terminal_id_.empty()) {
            terminal_id_ = new_terminal_identity();
            if (terminal_id_.empty()) return reject(error, "terminal_identity_unavailable");
        }
        peer_ready_ = true;
        // Open also resumes an existing surface after its local pipe changed.
        // Its last output ACK may have died with that pipe; resend the bounded
        // pending chunk so the Controller can parse or deduplicate and re-ACK.
        output_sent_ = false;
        if (!start_attempted_) {
            if (!input_eligible_) {
                error_code_ = "terminal_desktop_unavailable";
                send_state(TerminalMessageTypeV1::kReady);
                return reject(error, "terminal_input_unavailable");
            }
            error_code_.clear(); start_attempted_ = true;
            if (!terminal_.start(working_directory_, {static_cast<std::uint16_t>(message.columns),
                static_cast<std::uint16_t>(message.rows)}, &error_code_)) exited_ = true;
        }
        send_state(TerminalMessageTypeV1::kReady);
        return true;
    }
    if (message.type == TerminalMessageTypeV1::kEnd) {
        if (message.client_session_id != client_session_id_ || terminal_id_.empty()
            || (!message.terminal_id.empty() && message.terminal_id != terminal_id_)) return reject(error, "terminal_stale_identity");
        terminal_.stop(); pending_output_.reset(); output_sent_ = pending_was_sent_ = false;
        ended_ = exited_ = end_ack_pending_ = true; state_dirty_ = false;
        error_code_ = "terminal_session_ended";
        if (error) error->clear(); return true;
    }
    if (terminal_id_.empty() || message.terminal_id != terminal_id_) return reject(error, "terminal_stale_identity");
    switch (message.type) {
    case TerminalMessageTypeV1::kInput:
        if (!peer_ready_) return reject(error, "terminal_not_ready");
        if (message.input_generation != input_generation_) return reject(error, "terminal_stale_input_generation");
        if (message.sequence <= input_sequence_) return true; // no command replay
        if (message.sequence != input_sequence_ + 1) return reject(error, "terminal_input_sequence_gap");
        input_sequence_ = message.sequence;
        state_dirty_ = true; // acknowledge accepted input before the next GUI chunk
        // Consume, but never retain, input arriving during a transfer or pause.
        if (!input_eligible_ || exited_) return reject(error, "terminal_input_unavailable");
        if (!terminal_.write(message.bytes)) {
            error_code_ = "terminal_input_backpressure"; state_dirty_ = true;
            return reject(error, "terminal_input_backpressure");
        }
        break;
    case TerminalMessageTypeV1::kOutputAck:
        if (message.sequence <= consumed_sequence_) return true;
        if (!pending_output_ || !pending_was_sent_ || message.sequence != pending_output_->sequence) {
            return reject(error, "terminal_invalid_output_ack");
        }
        consumed_sequence_ = message.sequence;
        pending_output_.reset(); output_sent_ = pending_was_sent_ = false;
        break;
    case TerminalMessageTypeV1::kResize:
        if (!peer_ready_) return reject(error, "terminal_not_ready");
        if (message.input_generation != input_generation_) return reject(error, "terminal_stale_input_generation");
        if (!input_eligible_ || exited_) return reject(error, "terminal_input_unavailable");
        if (!terminal_.resize({static_cast<std::uint16_t>(message.columns), static_cast<std::uint16_t>(message.rows)})) {
            return reject(error, "terminal_resize_unavailable");
        }
        break;
    default:
        return reject(error, "terminal_wrong_direction");
    }
    if (error) error->clear();
    return true;
}
void TerminalHost::pump() {
    if (terminal_id_.empty()) return;
    if (end_ack_pending_ && connected_) {
        send_state(TerminalMessageTypeV1::kEnded);
        end_ack_pending_ = state_dirty_;
    }
    if (ended_) return;
    if (!peer_ready_) return; // await Open before sending retained output to a new surface
    if (state_dirty_) send_state(TerminalMessageTypeV1::kState);
    if (!connected_ || state_dirty_) return;
    if (!pending_output_) {
        if (auto bytes = terminal_.take_output()) {
            protocol::TerminalMessageV1 message;
            message.type = TerminalMessageTypeV1::kOutput; message.session_epoch = epoch_;
            message.terminal_id = terminal_id_; message.sequence = consumed_sequence_ + 1;
            message.bytes = std::move(*bytes);
            pending_output_ = std::move(message);
        }
    }
    if (pending_output_ && !output_sent_ && sender_) {
        output_sent_ = sender_(*pending_output_);
        pending_was_sent_ = pending_was_sent_ || output_sent_;
    }
    if (!pending_output_ && !exited_ && terminal_.output_finished()) {
        exited_ = true;
        terminal_.stop(); // terminate remaining descendants of the exited shell
        send_state(TerminalMessageTypeV1::kState);
    }
}
void TerminalHost::end_session() {
    terminal_.stop(); epoch_.clear(); terminal_id_.clear(); working_directory_.clear(); error_code_.clear();
    pending_output_.reset(); consumed_sequence_ = 0; input_sequence_ = 0; input_generation_ = 0;
    connected_ = input_eligible_ = exited_ = output_sent_ = pending_was_sent_ = state_dirty_ = false;
    start_attempted_ = false;
    peer_ready_ = false;
    ended_ = end_ack_pending_ = false; client_session_id_.clear();
}
bool TerminalHost::running() const { return terminal_.running(); }
std::size_t TerminalHost::buffered_output_bytes() const {
    return terminal_.buffered_output_bytes() + (pending_output_ ? pending_output_->bytes.size() : 0);
}
std::string_view TerminalHost::terminal_id() const { return terminal_id_; }
}
