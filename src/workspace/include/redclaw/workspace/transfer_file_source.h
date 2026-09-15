#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace redclaw::workspace {
enum class TransferReadState { kChunk, kComplete, kFailed };
struct TransferReadResult {
    TransferReadState state = TransferReadState::kFailed;
    std::uint64_t offset = 0;
    std::span<const std::uint8_t> bytes; // valid until the next read or close
    std::string sha256;
    std::string error;
};
// One transfer worker owns this file and its reusable 64 KiB read buffer. The
// handle excludes ordinary writers/deletion and is rechecked before completion.
class TransferFileSource final {
public:
    TransferFileSource();
    ~TransferFileSource();
    bool open(const std::filesystem::path& path, std::string* error = nullptr);
    TransferReadResult read(std::size_t max_bytes = 64U * 1024U);
    void close();
    [[nodiscard]] std::uint64_t size() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
