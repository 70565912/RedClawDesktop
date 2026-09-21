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
std::optional<ClipboardFocusToken> ClipboardHostPaste::snapshot(std::string* error) {
    return actions_.focus_snapshot ? actions_.focus_snapshot() : focus_ ? focus_->snapshot(error) : std::nullopt;
}
bool ClipboardHostPaste::begin(std::string epoch, std::string operation, std::string* error) {
    if (!operation_.empty() || !actions_.eligibility || !actions_.submit_paste) return fail(error, "clipboard_input_unavailable");
    const auto eligibility = actions_.eligibility();
    if (!eligibility.eligible) return fail(error, "clipboard_input_revoked");
    if (!actions_.focus_snapshot) focus_ = std::make_unique<ClipboardFocusTracker>();
    const auto focus = snapshot(error);
    if (!focus) { cancel(); return false; }
    if (!guard_.begin(epoch, operation, *focus, eligibility.revision, true)) { cancel(); return fail(error, "clipboard_paste_invalid_start"); }
    epoch_ = std::move(epoch); operation_ = std::move(operation);
    if (error) error->clear(); return true;
}
bool ClipboardHostPaste::pump() {
    if (operation_.empty()) return true;
    const auto eligibility = actions_.eligibility();
    return guard_.eligible(eligibility.revision, eligibility.eligible) && (actions_.focus_snapshot || focus_->pump_events());
}
void ClipboardHostPaste::cancel() { guard_.cancel(); focus_.reset(); epoch_.clear(); operation_.clear(); }
bool ClipboardHostPaste::complete(PreparedClipboardPayload& payload, std::string* error) {
    if (operation_.empty() || !guard_.verified(epoch_, operation_)) return fail(error, "clipboard_paste_not_pending");
    std::optional<ClipboardFocusToken> authorized_focus;
    std::uint64_t revision = 0;
    std::string guard_error;
    const auto publish_guard = [&] {
        const auto eligibility = actions_.eligibility();
        const auto focus = snapshot();
        const auto decision = guard_.take(epoch_, operation_, focus, eligibility.revision, eligibility.eligible);
        if (decision != ClipboardPasteDecision::kSubmitOnce) { guard_error = reason(decision); return false; }
        authorized_focus = focus; revision = eligibility.revision; return true;
    };
    const bool published = actions_.publish ? actions_.publish(payload, publish_guard, error) : payload.publish(publish_guard, error);
    if (!published) { if (!guard_error.empty() && error) *error = guard_error; cancel(); return false; }
    // Opening/replacing the clipboard can notify another application. Recheck
    // focus and independent input policy after those callbacks, before keys.
    const auto eligibility = actions_.eligibility();
    const auto current = snapshot();
    if (!current || current != authorized_focus) { cancel(); return fail(error, "clipboard_focus_changed"); }
    if (!eligibility.eligible || revision != eligibility.revision) { cancel(); return fail(error, "clipboard_input_revoked"); }
    const bool result = actions_.submit_paste(revision, error);
    cancel(); return result;
}
bool ClipboardHostPaste::publish(PreparedClipboardPayload& payload, std::string* error) {
    const auto eligible = [this] { return actions_.publish_eligible ? actions_.publish_eligible()
        : actions_.eligibility && actions_.eligibility().eligible; };
    if (!eligible()) return fail(error, "clipboard_input_revoked");
    return actions_.publish ? actions_.publish(payload, eligible, error) : payload.publish(eligible, error);
}
bool ClipboardHostPaste::paste(std::string* error) {
    if (operation_.empty() || !guard_.verified(epoch_, operation_)) return fail(error, "clipboard_paste_not_pending");
    const auto eligibility = actions_.eligibility();
    const auto current = snapshot();
    const auto decision = guard_.take(epoch_, operation_, current, eligibility.revision, eligibility.eligible);
    if (decision != ClipboardPasteDecision::kSubmitOnce) { cancel(); return fail(error, reason(decision)); }
    const auto again = actions_.eligibility();
    if (snapshot() != current || !again.eligible || again.revision != eligibility.revision) {
        cancel(); return fail(error, "clipboard_focus_changed");
    }
    const bool result = actions_.submit_paste(eligibility.revision, error);
    cancel(); return result;
}
}
