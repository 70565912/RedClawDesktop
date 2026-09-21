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
    integration_.reset(new_terminal_identity());
}
void TerminalHost::set_connection(bool open, bool eligible) {
    if (connected_ == open && input_eligible_ == eligible) return;
    if (!open || !eligible) terminal_.discard_pending_input();
    if (!open) {
        peer_ready_ = false;
        if (!execution_id_.empty()) execution_result("unknown", "terminal_connection_lost");
    }
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
    if (capability_ >= 2) { state.prompt_ready = prompt_ready_; state.capability_version = capability_; state.operation_id = execution_id_; }
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
                static_cast<std::uint16_t>(message.rows)}, &error_code_, integration_.nonce())) exited_ = true;
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
    case TerminalMessageTypeV1::kExec:
        if (capability_ < 2) return reject(error, "terminal_peer_unsupported");
        {
        const auto deny = [&](const char* reason) {
            auto denied = message; denied.type = TerminalMessageTypeV1::kExecState;
            denied.bytes.clear(); denied.execution_state = "rejected"; denied.error_code = reason;
            denied.input_sequence = input_sequence_; denied.input_generation = input_generation_;
            execution_events_.push_back(std::move(denied)); state_dirty_ = true;
            return reject(error, reason);
        };
        if (message.input_generation != input_generation_ || !peer_ready_ || !input_eligible_ || exited_)
            return deny("terminal_input_unavailable");
        if (message.sequence <= input_sequence_) return true;
        if (message.sequence != input_sequence_ + 1) return deny("terminal_input_sequence_gap");
        input_sequence_ = message.sequence; state_dirty_ = true;
        if (!prompt_ready_ || !execution_id_.empty()) return deny("terminal_busy");
        execution_id_ = message.operation_id; cancel_requested_ = false; completion_.reset(); prompt_ready_ = false;
        if (!terminal_.write(integration_.execution_line(execution_id_, message.bytes)))
            execution_result("failed", "terminal_input_backpressure");
        }
        break;
    case TerminalMessageTypeV1::kCancel:
        if (capability_ < 2 || message.operation_id != execution_id_ || execution_id_.empty())
            return reject(error, "terminal_operation_unavailable");
        if (completion_ || cancel_requested_) return true;
        cancel_requested_ = true;
        if (!terminal_.write("\x03")) return reject(error, "terminal_input_backpressure");
        {
            auto state = message; state.type = TerminalMessageTypeV1::kExecState;
            state.execution_state = "cancelling"; execution_events_.push_back(std::move(state));
        }
        break;
    case TerminalMessageTypeV1::kInput:
        if (!peer_ready_) return reject(error, "terminal_not_ready");
        if (message.input_generation != input_generation_) return reject(error, "terminal_stale_input_generation");
        if (message.sequence <= input_sequence_) return true; // no command replay
        if (message.sequence != input_sequence_ + 1) return reject(error, "terminal_input_sequence_gap");
        input_sequence_ = message.sequence;
        state_dirty_ = true; // acknowledge accepted input before the next GUI chunk
        // Consume, but never retain, input arriving during a transfer or pause.
        if (!input_eligible_ || exited_) return reject(error, "terminal_input_unavailable");
        if (!execution_id_.empty()) {
            if (message.bytes == "\x03") {
                if (cancel_requested_ || completion_) return true;
                cancel_requested_ = true;
                return terminal_.write(message.bytes);
            }
            return reject(error, "terminal_busy");
        }
        prompt_ready_ = false;
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
    while (!execution_events_.empty()) {
        auto& event = execution_events_.front(); event.session_epoch = epoch_;
        if (!sender_ || !sender_(event)) return;
        execution_events_.pop_front();
    }
    const auto integration_bytes = terminal_.take_integration();
    if (integration_bytes) (void)integration_.consume(*integration_bytes, [this](const auto& event) { shell_event(event); });
    if (!integration_bytes && !execution_id_.empty() && !terminal_.integration_alive())
        execution_result("unknown", "terminal_completion_channel_lost");
    if (!pending_output_) {
        if (auto bytes = terminal_.take_output()) {
            output_position_ += bytes->size();
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
    if (!integration_bytes && !pending_output_ && execution_events_.empty() && !exited_ && terminal_.output_finished()) {
        exited_ = true;
        if (!execution_id_.empty()) execution_result("unknown", "terminal_shell_exited_without_completion");
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
    execution_id_.clear(); completion_.reset(); execution_events_.clear();
    prompt_ready_ = cancel_requested_ = false; output_position_ = 0;
}
void TerminalHost::execution_result(std::string state, std::string error) {
    protocol::TerminalMessageV1 message;
    message.type = TerminalMessageTypeV1::kExecState; message.session_epoch = epoch_; message.terminal_id = terminal_id_;
    message.operation_id = execution_id_; message.execution_state = std::move(state); message.error_code = std::move(error);
    message.output_position = completion_ ? completion_->output_position : output_position_;
    if (completion_) {
        message.powershell_success = completion_->success; message.has_native_exit_code = completion_->has_native_exit_code;
        message.last_native_exit_code = completion_->last_native_exit_code;
    }
    if (execution_events_.size() < 16) execution_events_.push_back(std::move(message));
    execution_id_.clear(); completion_.reset(); cancel_requested_ = false;
}
void TerminalHost::shell_event(const TerminalShellIntegration::Event& event) {
    if (event.kind == "ready") {
        prompt_ready_ = true; state_dirty_ = true;
        if (!execution_id_.empty()) execution_result(cancel_requested_ ? "cancelled"
            : completion_ ? (completion_->success ? "succeeded" : "failed") : "unknown",
            completion_ || cancel_requested_ ? "" : "terminal_completion_missing");
    } else if (!execution_id_.empty() && event.operation_id == execution_id_) {
        if (event.kind == "output" && !event.output.empty()) {
            if (!execution_events_.empty() && execution_events_.back().execution_state == "output"
                && execution_events_.back().bytes.size() + event.output.size() <= protocol::kMaxTerminalChunkBytes) {
                execution_events_.back().bytes += event.output;
            } else {
                protocol::TerminalMessageV1 message; message.type = TerminalMessageTypeV1::kExecState;
                message.session_epoch = epoch_; message.terminal_id = terminal_id_; message.operation_id = execution_id_;
                message.execution_state = "output"; message.bytes = event.output; message.sequence = 1;
                execution_events_.push_back(std::move(message));
            }
        } else if (event.kind == "done") completion_ = event;
        else if (event.kind == "start") {
            protocol::TerminalMessageV1 message; message.type = TerminalMessageTypeV1::kExecState;
            message.session_epoch = epoch_; message.terminal_id = terminal_id_; message.operation_id = execution_id_;
            message.execution_state = "running"; message.output_position = event.output_position;
            if (execution_events_.size() < 16) execution_events_.push_back(std::move(message));
        }
    }
}
bool TerminalHost::running() const { return terminal_.running(); }
std::size_t TerminalHost::buffered_output_bytes() const {
    return terminal_.buffered_output_bytes() + (pending_output_ ? pending_output_->bytes.size() : 0);
}
std::string_view TerminalHost::terminal_id() const { return terminal_id_; }
}
