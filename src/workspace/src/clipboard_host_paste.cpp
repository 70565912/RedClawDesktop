#include "redclaw/workspace/clipboard_host_paste.h"
#include <utility>

namespace redclaw::workspace {
namespace {
bool fail(std::string* error, const char* reason) { if (error) *error = reason; return false; }
const char* reason(ClipboardPasteDecision result) {
    switch (result) {
    case ClipboardPasteDecision::kNoPendingPaste: return "clipboard_paste_not_pending";
    case ClipboardPasteDecision::kStaleOperation: return "clipboard_operation_stale";
    case ClipboardPasteDecision::kDataNotVerified: return "clipboard_data_not_verified";
    case ClipboardPasteDecision::kFocusChanged: return "clipboard_focus_changed";
    case ClipboardPasteDecision::kInputRevoked: return "clipboard_input_revoked";
    case ClipboardPasteDecision::kSubmitOnce: return "";
    }
    return "clipboard_paste_invalidated";
}
}
ClipboardHostPaste::ClipboardHostPaste(ClipboardHostActions actions) : actions_(std::move(actions)) {}
bool ClipboardHostPaste::begin(std::string epoch, std::string operation, std::string* error) {
    if (focus_ || !actions_.eligibility || !actions_.submit_paste) return fail(error, "clipboard_input_unavailable");
    const auto eligibility = actions_.eligibility();
    if (!eligibility.eligible) return fail(error, "clipboard_input_revoked");
    focus_ = std::make_unique<ClipboardFocusTracker>();
    const auto focus = focus_->snapshot(error);
    if (!focus) { cancel(); return false; }
    if (!guard_.begin(epoch, operation, *focus, eligibility.revision, true)) { cancel(); return fail(error, "clipboard_paste_invalid_start"); }
    epoch_ = std::move(epoch); operation_ = std::move(operation);
    if (error) error->clear(); return true;
}
bool ClipboardHostPaste::pump() {
    if (!focus_) return true;
    const auto eligibility = actions_.eligibility();
    return guard_.eligible(eligibility.revision, eligibility.eligible) && focus_->pump_events();
}
void ClipboardHostPaste::cancel() { guard_.cancel(); focus_.reset(); epoch_.clear(); operation_.clear(); }
bool ClipboardHostPaste::complete(PreparedClipboardPayload& payload, std::string* error) {
    if (!focus_ || !guard_.verified(epoch_, operation_)) return fail(error, "clipboard_paste_not_pending");
    std::optional<ClipboardFocusToken> authorized_focus;
    std::uint64_t revision = 0;
    std::string guard_error;
    const bool published = payload.publish([&] {
        const auto eligibility = actions_.eligibility();
        const auto focus = focus_->snapshot();
        const auto decision = guard_.take(epoch_, operation_, focus, eligibility.revision, eligibility.eligible);
        if (decision != ClipboardPasteDecision::kSubmitOnce) { guard_error = reason(decision); return false; }
        authorized_focus = focus; revision = eligibility.revision; return true;
    }, error);
    if (!published) { if (!guard_error.empty() && error) *error = guard_error; cancel(); return false; }
    // Opening/replacing the clipboard can notify another application. Recheck
    // focus and independent input policy after those callbacks, before keys.
    const auto eligibility = actions_.eligibility();
    const auto current = focus_->snapshot();
    if (!current || current != authorized_focus) { cancel(); return fail(error, "clipboard_focus_changed"); }
    if (!eligibility.eligible || revision != eligibility.revision) { cancel(); return fail(error, "clipboard_input_revoked"); }
    const bool result = actions_.submit_paste(revision, error);
    cancel(); return result;
}
}
