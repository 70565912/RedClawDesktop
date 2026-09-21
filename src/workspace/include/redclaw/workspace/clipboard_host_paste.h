#pragma once
#include "redclaw/workspace/clipboard_paste_guard.h"
#include "redclaw/workspace/clipboard_payload.h"
#include <functional>

namespace redclaw::workspace {
struct ClipboardInputEligibility {
    bool eligible = false;
    std::uint64_t revision = 0;
};
struct ClipboardHostActions {
    std::function<ClipboardInputEligibility()> eligibility;
    std::function<bool(std::uint64_t expected_revision, std::string* error)> submit_paste;
    std::function<std::optional<ClipboardFocusToken>()> focus_snapshot;
    std::function<bool(PreparedClipboardPayload&, const std::function<bool()>&, std::string*)> publish;
    std::function<bool()> publish_eligible;
    ClipboardCapture capture;
};
// Runtime-owner component. Expensive disk reads/allocation have already finished
// on TransferWorker; final policy, focus and fixed input submission share the
// same owner thread as the ordinary Host remote-input session.
class ClipboardHostPaste final {
public:
    explicit ClipboardHostPaste(ClipboardHostActions actions);
    bool begin(std::string epoch, std::string operation, std::string* error);
    bool pump();
    bool complete(PreparedClipboardPayload& payload, std::string* error);
    bool paste(std::string* error);
    bool publish(PreparedClipboardPayload& payload, std::string* error);
    void cancel();
private:
    ClipboardHostActions actions_;
    ClipboardPasteGuard guard_;
    std::unique_ptr<ClipboardFocusTracker> focus_;
    std::string epoch_, operation_;
    std::optional<ClipboardFocusToken> snapshot(std::string* error = nullptr);
};
}
