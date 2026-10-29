#pragma once
#include "redclaw/workspace/clipboard_payload.h"
#include "redclaw/workspace/transfer_source_manifest.h"
#include <vector>

namespace redclaw::workspace {
inline constexpr std::size_t kMaxClipboardDescriptionEntries = 65536;
inline constexpr std::size_t kMaxClipboardDescriptionBytes = 64U * 1024U * 1024U;
struct ClipboardDescribedEntry {
    std::string relative_path, staged_path;
    std::uint64_t size = 0;
    bool directory = false;
};
struct ClipboardSourceIdentity {
    std::filesystem::path path; // source-local only, never serialized
    std::uint64_t volume = 0, file = 0, size = 0, written = 0;
    bool directory = false;
};
struct ClipboardDescription {
    std::uint32_t roots = 0;
    TransferScanProgress totals;
    std::vector<ClipboardDescribedEntry> entries;
    std::vector<ClipboardSourceIdentity> identities;
    std::string serialize() const;
    static std::shared_ptr<ClipboardDescription> parse(std::string_view bytes, std::string* error);
    bool sources_unchanged(const std::atomic_bool& cancelled, std::string* error) const;
};
bool describe_clipboard_files(std::span<const ClipboardFileSelection> files, const std::filesystem::path& spool,
    const std::atomic_bool& cancelled, ClipboardDescription& result, std::string* error);
}
