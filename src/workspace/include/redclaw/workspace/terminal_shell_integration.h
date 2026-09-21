#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace redclaw::workspace {
// Parses only fixed, nonce-bound Shell integration records; ordinary VT output
// stays byte-exact. A partial record retains at most 8192 bytes.
class TerminalShellIntegration final {
public:
    struct Event {
        std::string kind, operation_id;
        std::string output;
        std::uint64_t output_position = 0;
        bool success = false, has_native_exit_code = false;
        std::int32_t last_native_exit_code = 0;
    };
    void reset(std::string nonce);
    std::string consume(std::string_view bytes, const std::function<void(const Event&)>& event);
    [[nodiscard]] std::string execution_line(std::string_view id, std::string_view utf8) const;
    [[nodiscard]] std::string_view nonce() const { return nonce_; }
private:
    std::string nonce_, pending_;
    std::uint64_t position_ = 0;
};
}
