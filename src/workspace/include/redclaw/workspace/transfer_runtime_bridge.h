#pragma once
#include "redclaw/protocol/stream_control_protocol.h"
#include "redclaw/workspace/transfer_operation_gate.h"
#include "redclaw/workspace/transfer_worker.h"
#include "redclaw/workspace/directory_browser.h"
#include "redclaw/workspace/clipboard_host_paste.h"
#include <functional>

namespace redclaw::workspace {
#ifdef _WIN32
inline constexpr std::uint32_t kFileTransferCapabilityVersion = 1;
#else
inline constexpr std::uint32_t kFileTransferCapabilityVersion = 0;
#endif
inline constexpr std::uint32_t kClipboardCapabilityVersion = kFileTransferCapabilityVersion ? 2 : 0;
// Coordinates one file batch on the existing Control v1 envelope. Callbacks
// only enqueue bounded data; all transitions, control sends, and gate updates
// belong to the runtime owner. Disk work belongs exclusively to TransferWorker.
class TransferRuntimeBridge final {
public:
    using ControlSend = std::function<bool(const protocol::StreamControlMessageV1&)>;
    using BulkSend = std::function<bool(std::string_view)>;
    TransferRuntimeBridge(bool host, std::string local_epoch, std::filesystem::path spool_directory,
        TransferOperationGate& gate, ControlSend peer, ControlSend gui, BulkSend bulk, std::function<bool()> ensure,
        ClipboardHostActions clipboard_actions = {});
    void peer_capability(std::uint32_t version, std::string epoch, std::uint32_t clipboard_version = 0);
    void channel_open(bool open);
    bool receive_control(const protocol::StreamControlMessageV1& message); // after the shared replay guard
    bool receive_bulk(std::string_view frame);
    void from_gui(const protocol::StreamControlMessageV1& message); // owner only
    void pump(std::uint64_t now_ms, bool connection_ready);
    void reset_transport(std::string local_epoch); // owner after transport callback drain
private:
    using Action = protocol::WorkspaceActionV1;
    using Control = protocol::StreamControlMessageV1;
    Control make(Action action, std::string operation = {}) const;
    void queue(Control message);
    void local(Control message);
    void peer_message(const Control& message);
    bool begin(const Control& message);
    void cancel(std::string error, bool notify_peer);
    void disconnect();
    void finish_local();
    void create_source();
    void create_receiver(const protocol::WorkspaceControlV1& offer);
    void create_copy_action();
    void update_worker(std::uint64_t now);
    void update_availability();
    void browse(const Control& request);
    void pump_browser();
    bool matches(const Control& message) const;
    bool source() const;
    bool clipboard() const { return purpose_ == protocol::WorkspaceTransferPurposeV1::kClipboard; }
    bool copy_action() const { return purpose_ == protocol::WorkspaceTransferPurposeV1::kClipboardCleanup
        || purpose_ == protocol::WorkspaceTransferPurposeV1::kClipboardOpenCopy; }
    bool host_role_;
    std::string local_epoch_, peer_epoch_, operation_epoch_, operation_, error_;
    std::filesystem::path spool_directory_, destination_;
    TransferOperationGate& gate_;
    ControlSend peer_send_, gui_send_;
    BulkSend bulk_send_;
    std::function<bool()> ensure_;
    std::unique_ptr<TransferWorker> worker_;
    ClipboardHostPaste clipboard_paste_;
    std::unique_ptr<DirectoryBrowser> browser_;
    std::string browse_operation_;
    std::optional<Control> pending_browse_, browse_reply_;
    protocol::TransferDirectionV1 direction_ = protocol::TransferDirectionV1::kToHost;
    protocol::WorkspaceTransferPurposeV1 purpose_ = protocol::WorkspaceTransferPurposeV1::kFiles;
    std::uint32_t clipboard_sequence_ = 0, clipboard_version_ = 0;
    std::string clipboard_batch_id_;
    bool clipboard_ready_ = false, paste_submitted_ = false, paste_requested_ = false;
    protocol::TransferConflictV1 conflict_ = protocol::TransferConflictV1::kKeepBoth;
    std::deque<Control> outgoing_;
    std::optional<Control> progress_message_;
    std::optional<std::string> pending_bulk_;
    std::mutex inbox_mutex_;
    std::deque<Control> controls_;
    std::deque<std::string> bulk_;
    std::uint32_t peer_version_ = 0;
    std::uint32_t peer_clipboard_version_ = 0;
    std::string advertised_peer_epoch_;
    bool open_ = false, overflow_ = false;
    std::uint64_t invalidation_ = 0, observed_invalidation_ = 0;
    bool ready_ = false, prepared_ = false, offered_ = false, transfer_ready_ = false;
    bool cancelled_ = false, local_finished_ = false, completed_notification_queued_ = false;
    std::uint64_t next_ensure_ms_ = 0, next_progress_ms_ = 0, accepted_sources_ = 0;
    unsigned ensure_attempts_ = 0;
    bool availability_known_ = false, notified_ready_ = false, notified_busy_ = false;
    std::uint32_t notified_clipboard_version_ = 0;
    std::uint64_t notified_revision_ = 0;
};
}
