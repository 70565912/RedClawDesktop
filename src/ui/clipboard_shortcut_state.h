#pragma once

#include <cstdint>
#include <optional>

namespace redclaw::ui {

class ClipboardShortcutState {
public:
    void remote_copy(std::uint32_t local_sequence) { remote_copy_sequence_ = local_sequence; }
    void reset() { remote_copy_sequence_.reset(); }
    [[nodiscard]] bool should_transfer_local_clipboard(std::uint32_t local_sequence) const {
        return !remote_copy_sequence_ || *remote_copy_sequence_ != local_sequence;
    }

private:
    std::optional<std::uint32_t> remote_copy_sequence_;
};

}  // namespace redclaw::ui
