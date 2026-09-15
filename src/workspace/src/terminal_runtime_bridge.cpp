#include "redclaw/workspace/terminal_runtime_bridge.h"
#include <charconv>
#include <cstdlib>
#include <limits>

namespace redclaw::workspace {
using protocol::TerminalMessageTypeV1;
TerminalRuntimeBridge::TerminalRuntimeBridge(bool host, std::string epoch,
    std::filesystem::path directory, Send send, EnsureChannel ensure, DesktopAllowed allowed)
    : host_role_(host), send_(std::move(send)), ensure_(std::move(ensure)),
      desktop_allowed_(std::move(allowed)), host_([this](const auto& message) { return send_peer(message); }),
      epoch_(std::move(epoch)) {
    if (host_role_) host_.begin_session(epoch_, std::move(directory));
}
void TerminalRuntimeBridge::connect_gui_from_environment() {
    const char* name = std::getenv("REDCLAW_WORKSPACE_PIPE_NAME");
    const char* owner = std::getenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID");
    if (!name || !owner) return;
    std::uint32_t pid = 0;
    const std::string_view text(owner);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), pid);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !pid) return;
    (void)gui_.connect(name, pid);
}
void TerminalRuntimeBridge::peer_capability(std::uint32_t version, std::string epoch) {
    std::lock_guard lock(inbox_mutex_);
    peer_version_ = kTerminalCapabilityVersion && version >= 1 ? 1 : 0;
    peer_epoch_ = std::move(epoch);
}
void TerminalRuntimeBridge::channel_open(bool open) {
    std::lock_guard lock(inbox_mutex_);
    open_ = open;
    if (!open) { inbox_.clear(); ++channel_invalidation_; }
}
bool TerminalRuntimeBridge::receive(std::string_view frame) {
    auto parsed = protocol::parse_terminal_message_v1(frame);
    if (!parsed.ok) return false;
    const bool from_host = parsed.value.type == TerminalMessageTypeV1::kReady
        || parsed.value.type == TerminalMessageTypeV1::kState || parsed.value.type == TerminalMessageTypeV1::kOutput
        || parsed.value.type == TerminalMessageTypeV1::kEnded;
    if (parsed.value.type == TerminalMessageTypeV1::kAvailability || from_host == host_role_) return false;
    std::lock_guard lock(inbox_mutex_);
    if (!open_ || !peer_version_ || overflow_) return false;
    if (inbox_.size() >= 8) { overflow_ = true; inbox_.clear(); return false; }
    inbox_.push_back(std::move(parsed.value));
    return true;
}
void TerminalRuntimeBridge::reset_transport(std::string epoch) {
    {
        std::lock_guard lock(inbox_mutex_);
        inbox_.clear(); peer_version_ = 0; peer_epoch_.clear(); open_ = overflow_ = false;
        channel_invalidation_ = observed_invalidation_ = 0;
    }
    active_ = false; notified_ = false; next_ensure_ms_ = 0; ensure_attempts_ = 0;
    notify_offline_ = !host_role_;
    epoch_ = std::move(epoch);
    if (host_role_) { host_.set_connection(false, false); host_.rebind_epoch(epoch_); }
}
bool TerminalRuntimeBridge::send_peer(const protocol::TerminalMessageV1& message) {
    if (!active_ || !send_) return false;
    const auto bytes = protocol::serialize_terminal_message_v1(message);
    return !bytes.empty() && send_(bytes);
}
void TerminalRuntimeBridge::from_gui(std::string_view frame) {
    if (host_role_) return;
    const auto parsed = protocol::parse_terminal_message_v1(frame);
    if (!parsed.ok || !active_) return;
    const auto& message = parsed.value;
    if (message.session_epoch != epoch_) return;
    if (message.type != TerminalMessageTypeV1::kOpen && message.type != TerminalMessageTypeV1::kInput
        && message.type != TerminalMessageTypeV1::kResize && message.type != TerminalMessageTypeV1::kOutputAck
        && message.type != TerminalMessageTypeV1::kEnd) return;
    if (message.type != TerminalMessageTypeV1::kOutputAck && message.type != TerminalMessageTypeV1::kEnd && !input_allowed_) return;
    if (!send_peer(message)) {
        // An unacknowledged keystroke is never queued for a later connection.
        active_ = false; notified_ = false;
        std::lock_guard lock(inbox_mutex_);
        overflow_ = true;
    }
}
void TerminalRuntimeBridge::pump(std::uint64_t now, bool ready, bool allowed, std::uint64_t transfer_revision) {
    if (transfer_revision != observed_transfer_revision_) {
        observed_transfer_revision_ = transfer_revision;
        if (host_role_) host_.set_connection(active_, false);
        else notify_input_pause_ = true;
    }
    const bool local_paused = !allowed || notify_input_pause_;
    bool open, overflow;
    std::uint32_t version;
    std::uint64_t invalidation;
    std::string peer_epoch;
    {
        std::lock_guard lock(inbox_mutex_);
        open = open_; overflow = overflow_; version = peer_version_; peer_epoch = peer_epoch_;
        invalidation = channel_invalidation_;
    }
    if (invalidation != observed_invalidation_) {
        observed_invalidation_ = invalidation;
        if (host_role_) host_.set_connection(false, false);
        else { notify_offline_ = true; notified_ = false; }
    }
    if (open) ensure_attempts_ = 0;
    if (host_role_ && ready && version && !open && !overflow && ensure_attempts_ < 3 && now >= next_ensure_ms_) {
        ++ensure_attempts_;
        next_ensure_ms_ = now + 3000;
        if (ensure_) (void)ensure_();
    }
    active_ = ready && version && open && !overflow && !notify_offline_;
    if (!host_role_ && !peer_epoch.empty()) epoch_ = std::move(peer_epoch);
    if (host_role_ && host_.running() && now >= next_desktop_probe_ms_) {
        desktop_ok_ = desktop_allowed_ && desktop_allowed_();
        next_desktop_probe_ms_ = now + 500;
    }
    input_allowed_ = active_ && !local_paused && (!host_role_ || desktop_ok_);
    if (host_role_) host_.set_connection(active_, input_allowed_);
    // Polling drains stale local input even while the network is unavailable.
    gui_.poll([this](auto frame) { from_gui(frame); });
    if (!host_role_ && gui_.writable()
        && (!notified_ || notified_active_ != active_ || notified_epoch_ != epoch_
            || notified_input_paused_ != local_paused)) {
        protocol::TerminalMessageV1 status;
        status.type = TerminalMessageTypeV1::kAvailability;
        status.session_epoch = epoch_; status.input_enabled = active_;
        status.local_input_paused = local_paused;
        if (!active_) status.error_code = overflow ? "terminal_receive_overflow"
            : (ready && !version ? "terminal_peer_unsupported" : "terminal_connection_unavailable");
        if (gui_.send(protocol::serialize_terminal_message_v1(status))) {
            notified_ = true; notified_active_ = active_; notified_epoch_ = epoch_;
            notified_input_paused_ = local_paused;
            notify_input_pause_ = false;
            notify_offline_ = false;
        }
    }
    for (unsigned count = 0; count < 8 && active_; ++count) {
        protocol::TerminalMessageV1 message;
        {
            std::lock_guard lock(inbox_mutex_);
            if (inbox_.empty() || (!host_role_ && !gui_.writable())) break;
            message = std::move(inbox_.front()); inbox_.pop_front();
        }
        if (message.session_epoch != epoch_) continue;
        if (host_role_) {
            if (message.type == TerminalMessageTypeV1::kOpen || message.type == TerminalMessageTypeV1::kInput
                || message.type == TerminalMessageTypeV1::kResize) {
                desktop_ok_ = desktop_allowed_ && desktop_allowed_();
                host_.set_connection(active_, allowed && desktop_ok_);
            }
            (void)host_.receive(message);
        } else {
            if (!gui_.send(protocol::serialize_terminal_message_v1(message))) {
                std::lock_guard lock(inbox_mutex_);
                inbox_.push_front(std::move(message));
                break;
            }
        }
    }
    if (host_role_) host_.pump();
}
}
