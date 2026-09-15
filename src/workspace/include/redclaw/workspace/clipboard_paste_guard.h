#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace redclaw::workspace {
// Host-local identity only. Never serialized or disclosed to the peer.
struct ClipboardFocusToken {
    std::uintptr_t foreground = 0, focus = 0;
    std::uint32_t process = 0, thread = 0, session = 0;
    std::uint64_t process_created = 0, generation = 0;
    bool operator==(const ClipboardFocusToken&) const = default;
};
// Own on the runtime thread, whose message queue also receives focus
// notifications. A focus change followed by a return still changes generation.
class ClipboardFocusTracker final {
public:
    ClipboardFocusTracker();
    ~ClipboardFocusTracker();
    [[nodiscard]] std::optional<ClipboardFocusToken> snapshot(std::string* error = nullptr);
    bool pump_events();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
enum class ClipboardPasteDecision {
    kSubmitOnce, kNoPendingPaste, kStaleOperation, kDataNotVerified, kFocusChanged, kInputRevoked
};
// Additional clipboard-specific latch within the existing transfer operation,
// not a second transfer state machine. The owner checks its independent input
// policy and revision; the transfer's temporary pause does not revoke consent.
class ClipboardPasteGuard final {
public:
    bool begin(std::string epoch, std::string operation, ClipboardFocusToken focus,
        std::uint64_t input_revision, bool eligible);
    bool verified(std::string_view epoch, std::string_view operation);
    ClipboardPasteDecision take(std::string_view epoch, std::string_view operation,
        const std::optional<ClipboardFocusToken>& focus, std::uint64_t input_revision, bool eligible);
    void cancel();
    [[nodiscard]] bool pending() const { return expected_.has_value(); }
    [[nodiscard]] bool eligible(std::uint64_t revision, bool allowed) const {
        return expected_ && allowed && expected_->input_revision == revision;
    }
private:
    struct Expected {
        std::string epoch, operation;
        ClipboardFocusToken focus;
        std::uint64_t input_revision = 0;
        bool verified = false;
    };
    std::optional<Expected> expected_;
};
}
