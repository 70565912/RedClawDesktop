#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace redclaw::workspace {
struct TransferSourceEntry {
    std::filesystem::path source;
    std::string relative_path;
    std::uint64_t size = 0;
    bool directory = false;
};
struct TransferScanProgress {
    std::uint64_t entries = 0, files = 0, bytes = 0;
};
enum class TransferManifestRead { kEntry, kEnd, kFailed };
// One worker scans into an exclusive delete-on-close spool, then streams its
// entries. Memory holds one record, not the complete directory tree. No source
// content is read during scanning; each file is revalidated when opened later.
class TransferSourceManifest final {
public:
    TransferSourceManifest();
    ~TransferSourceManifest();
    bool begin_scan(const std::filesystem::path& spool_directory, std::string* error = nullptr);
    bool add_source(const std::filesystem::path& source,
        const std::function<bool(const TransferScanProgress&)>& progress = {}, std::string* error = nullptr,
        std::string_view relative_root = {});
    bool finish_scan(std::string* error = nullptr);
    bool scan(std::span<const std::filesystem::path> selected,
        const std::filesystem::path& spool_directory,
        const std::function<bool(const TransferScanProgress&)>& progress, std::string* error = nullptr);
    TransferManifestRead next(TransferSourceEntry* entry, std::string* error = nullptr);
    void close(); // worker only; releases the owned spool, retaining final totals
    void request_cancel() noexcept;
    [[nodiscard]] TransferScanProgress totals() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic_bool cancelled_{false};
};
}
