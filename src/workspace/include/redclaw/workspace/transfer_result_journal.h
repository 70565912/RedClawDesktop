#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace redclaw::workspace {
struct TransferResultEntry {
    std::string relative_path;
    std::uint64_t size = 0;
    bool directory = false, skipped = false;
};
struct TransferResultPage {
    std::vector<TransferResultEntry> entries;
    std::uint64_t begin = 0, end = 0;
    bool has_previous = false, has_next = false;
    std::string error;
};
// Disk-worker owned. Records survive cancellation; no source paths or file
// contents enter this owner-local journal. Finish closes it before publication.
class TransferResultJournal final {
public:
    TransferResultJournal();
    ~TransferResultJournal();
    bool begin(const std::filesystem::path& directory, std::string* error);
    bool append(const TransferResultEntry& entry, std::string* error);
    void finish();
    [[nodiscard]] const std::filesystem::path& path() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Read only on a disk worker. Byte cursors and bidirectional record lengths
// permit bounded paging without an index proportional to the number of files.
inline constexpr std::size_t kTransferResultPageEntries = 128;
[[nodiscard]] TransferResultPage read_transfer_result_page(const std::filesystem::path& path,
    std::uint64_t cursor = 0, bool previous = false);
}
