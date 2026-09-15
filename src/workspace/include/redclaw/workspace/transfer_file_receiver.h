#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace redclaw::workspace {
enum class TransferConflict { kKeepBoth, kOverwrite, kSkip };
enum class TransferEntryResult { kReceiving, kSkipped, kCommitted, kFailed };
struct TransferFileResult {
    TransferEntryResult result = TransferEntryResult::kFailed;
    std::filesystem::path relative_path;
    std::uint64_t bytes = 0;
    std::string error;
};

// Worker-thread-only per-file transaction. Writes never touch the destination
// until size and SHA256 match. Cancellation deletes only the owned temporary
// handle; previously committed files and created directories are retained.
class TransferFileReceiver final {
public:
    TransferFileReceiver();
    ~TransferFileReceiver();
    TransferFileReceiver(const TransferFileReceiver&) = delete;
    TransferFileReceiver& operator=(const TransferFileReceiver&) = delete;
    bool begin_batch(const std::filesystem::path& destination, std::string operation_id, std::string* error = nullptr);
    bool directory(std::string_view relative_utf8, std::string* error = nullptr);
    TransferFileResult begin_file(std::string_view relative_utf8, std::uint64_t size, TransferConflict conflict);
    bool write(std::uint64_t offset, std::span<const std::uint8_t> bytes, std::string* error = nullptr);
    TransferFileResult commit(std::string_view expected_sha256);
    bool cancel_file(std::string* error = nullptr);
    bool end_batch(std::string* error = nullptr);
    [[nodiscard]] bool receiving() const;
    [[nodiscard]] std::uint64_t received_bytes() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool validate_transfer_relative_path(std::string_view utf8, std::filesystem::path* path, std::string* error = nullptr);
}
