#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace redclaw::workspace {

struct TerminalSize {
    std::uint16_t columns = 100;
    std::uint16_t rows = 30;
};

// One terminal belongs to one desktop session. The caller controls connection
// eligibility; this object never elevates or outlives explicit session shutdown.
// Public methods are called by the owning session thread. Pipe workers do not
// invoke callbacks into that owner.
class TerminalSession final {
public:
    TerminalSession();
    ~TerminalSession();
    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    bool start(const std::filesystem::path& working_directory, TerminalSize size,
               std::string* error, std::string_view integration_nonce = {});
    bool write(std::string_view utf8);
    bool resize(TerminalSize size);
    void discard_pending_input();
    [[nodiscard]] std::optional<std::string> take_output();
    [[nodiscard]] std::optional<std::string> take_integration();
    [[nodiscard]] bool integration_alive() const;
    [[nodiscard]] bool running() const;
    [[nodiscard]] std::size_t buffered_output_bytes() const;
    [[nodiscard]] bool output_finished() const;
    void stop();

    static constexpr std::size_t kMaxBufferedOutputBytes = 1024 * 1024;
    static constexpr std::size_t kMaxBufferedInputBytes = 64 * 1024;
    static constexpr std::size_t kOutputChunkBytes = 16 * 1024;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace redclaw::workspace
