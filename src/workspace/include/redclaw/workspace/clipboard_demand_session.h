#pragma once
#include "redclaw/workspace/clipboard_demand_port.h"
#include "redclaw/workspace/clipboard_host_paste.h"
#include "redclaw/workspace/transfer_worker.h"
#include "redclaw/protocol/stream_control_protocol.h"

namespace redclaw::workspace {
// Capability-5 policy/receipts around the existing transfer worker. It does not
// own another bulk channel or gate. Only the Controller starts a bulk operation.
class ClipboardDemandSession final {
public:
    using Control = protocol::StreamControlMessageV1;
    using Send = std::function<bool(const Control&)>;
    ClipboardDemandSession(bool host, std::string epoch, std::filesystem::path spool,
        std::shared_ptr<ClipboardDemandPort> port, ClipboardHostActions actions,
        Send peer, Send gui, Send start, std::function<void(std::string)> cancel,
        std::function<bool(std::string_view)> available);
    void pump(std::uint64_t now, bool enabled, std::string peer_epoch);
    bool from_gui(const Control& message);
    bool from_peer(const Control& message);
    void transfer_started(const Control& message);
    void received(std::string_view operation, std::shared_ptr<PreparedClipboardPayload> payload,
        std::filesystem::path directory, bool verified);
    void bulk_finished(const Control& result, bool source);
    bool reserved_for_other(std::string_view operation) const;
    bool accepts_transfer(const Control& message) const;
    std::uint32_t source_sequence(std::string_view token) const;
    ClipboardCapture checked_capture(std::string_view token, ClipboardCapture fallback) const;
    void disconnected();
    void reset_epoch(std::string epoch);
    void presentation(Control& message) const;
private:
    using Action = protocol::WorkspaceActionV1;
    struct Pending {
        std::string operation;
        ClipboardDemandOffer offer;
        protocol::TransferDirectionV1 direction = protocol::TransferDirectionV1::kToHost;
        bool demanded = false, asked = false, confirmed = false, read_requested = false, started = false, bulk_done = false;
        bool native_received = false, native_complete = false, committing = false;
        bool move_requested = false;
        std::uint32_t effect = 0;
        std::string native_error;
        std::shared_ptr<ClipboardDescription> description;
        protocol::WorkspaceControlV1 bulk;
    };
    Control message(Action action, std::string operation, ClipboardDemandOffer offer,
        protocol::TransferDirectionV1 direction) const;
    bool pending(std::string operation, ClipboardDemandOffer offer, protocol::TransferDirectionV1 direction);
    void describe(std::string operation, const ClipboardDemandOffer& offer);
    void demand(std::string operation, ClipboardDemandOffer offer, protocol::TransferDirectionV1 direction);
    void approve();
    void settle();
    void finish(std::string error = {}, bool notify_peer = false);
    void native(ClipboardDemandEvent event);
    void local_state(Action action, std::string status = {});
    bool matches(const Control& message) const;
    bool host_, enabled_ = false, native_failed_ = false;
    std::string epoch_, peer_epoch_;
    std::string shortcut_waiting_;
    std::filesystem::path spool_;
    std::shared_ptr<ClipboardDemandPort> port_;
    ClipboardHostActions actions_;
    ClipboardHostPaste shortcut_paste_;
    Send peer_, gui_, start_;
    std::function<void(std::string)> cancel_;
    std::function<bool(std::string_view)> available_;
    ClipboardDemandOffer local_offer_, remote_offer_;
    std::optional<Pending> pending_;
    std::unique_ptr<TransferWorker> preview_;
    std::string preview_operation_, preview_token_, sending_metadata_, receiving_metadata_;
    std::shared_ptr<ClipboardDescription> source_description_;
    std::uint32_t sending_page_ = 0, receiving_page_ = 0, receiving_pages_ = 0;
};
}
