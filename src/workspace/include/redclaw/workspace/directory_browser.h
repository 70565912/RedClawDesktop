#pragma once
#include "redclaw/protocol/transfer_protocol.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

namespace redclaw::workspace {
// Read-only, current-user directory enumeration. Results stream through a
// bounded mailbox; abandoning a browse cancels this worker before replacement.
class DirectoryBrowser final {
public:
    explicit DirectoryBrowser(std::filesystem::path directory, bool clipboard_copies = false);
    ~DirectoryBrowser();
    std::optional<protocol::WorkspaceControlV1> take();
    void cancel() noexcept;
    [[nodiscard]] bool finished() const { return finished_.load(std::memory_order_acquire); }
private:
    void run(std::filesystem::path directory, bool clipboard_copies);
    bool emit(protocol::WorkspaceControlV1 message);
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<protocol::WorkspaceControlV1> entries_;
    std::atomic_bool cancelled_{false}, finished_{false};
    std::thread thread_;
};
}
