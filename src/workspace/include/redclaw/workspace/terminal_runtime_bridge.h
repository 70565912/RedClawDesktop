#pragma once
#include "redclaw/workspace/local_workspace_pipe.h"
#include "redclaw/workspace/terminal_host.h"
#include <deque>
#include <mutex>

namespace redclaw::workspace {
#ifdef _WIN32
inline constexpr std::uint32_t kTerminalCapabilityVersion = 2;
#else
inline constexpr std::uint32_t kTerminalCapabilityVersion = 0;
#endif
// Network callbacks only deposit bounded messages. The existing runtime owner
// pumps ConPTY and GUI IPC; neither network nor media callbacks execute commands.
class TerminalRuntimeBridge final {
public:
    using Send = std::function<bool(std::string_view)>;
    using EnsureChannel = std::function<bool()>;
    using DesktopAllowed = std::function<bool()>;
    TerminalRuntimeBridge(bool host, std::string epoch, std::filesystem::path directory,
        Send send, EnsureChannel ensure, DesktopAllowed desktop_allowed);
    void connect_gui_from_environment();
    void peer_capability(std::uint32_t version, std::string epoch);
    void channel_open(bool open);
    bool receive(std::string_view frame);
    // Owner only, following the transport's retire-and-drain barrier.
    void reset_transport(std::string epoch);
    void pump(std::uint64_t now_ms, bool required_channels_ready, bool workspace_input_allowed = true,
        std::uint64_t transfer_revision = 0);
private:
    void from_gui(std::string_view frame);
    bool send_peer(const protocol::TerminalMessageV1& message);
    bool host_role_;
    Send send_;
    EnsureChannel ensure_;
    DesktopAllowed desktop_allowed_;
    TerminalHost host_;
    LocalWorkspacePipe gui_;
    std::mutex inbox_mutex_;
    std::deque<protocol::TerminalMessageV1> inbox_;
    std::uint32_t peer_version_ = 0;
    std::string peer_epoch_, epoch_;
    bool open_ = false, overflow_ = false, active_ = false;
    bool desktop_ok_ = false, input_allowed_ = false;
    bool notified_ = false, notified_active_ = false;
    bool notified_input_paused_ = false;
    std::string notified_epoch_;
    std::uint64_t next_ensure_ms_ = 0, next_desktop_probe_ms_ = 0;
    unsigned ensure_attempts_ = 0;
    std::uint64_t channel_invalidation_ = 0, observed_invalidation_ = 0;
    bool notify_offline_ = false;
    std::uint64_t observed_transfer_revision_ = 0;
    bool notify_input_pause_ = false;
};
}
