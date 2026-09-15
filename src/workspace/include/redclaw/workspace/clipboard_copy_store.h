#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace redclaw::workspace {
struct ClipboardCopyCleanupProgress {
    std::uint64_t files = 0, directories = 0;
};
// Disk-worker operations only. The peer chooses an opaque batch ID, never a
// deletion path. Directory handles pin every ancestor and reparse points are
// not traversed. The container holds only RedClaw-created received copies.
class ClipboardCopyStore final {
public:
    explicit ClipboardCopyStore(std::filesystem::path spool_directory);
    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
    [[nodiscard]] std::filesystem::path batch_directory(std::string_view id) const;
    static bool valid_id(std::string_view id);
    static std::string new_id();
    bool create_batch(std::string_view id, std::string* error);
    bool contains_batch(std::string_view id) const;
    bool open_batch(std::string_view id, std::string* error);
    bool cleanup_batch(std::string_view id, const std::atomic_bool& cancelled,
        const std::function<void(const ClipboardCopyCleanupProgress&)>& progress, std::string* error);
private:
    std::filesystem::path directory_;
};
}
