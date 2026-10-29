#include "redclaw/workspace/clipboard_description.h"
#include "redclaw/workspace/transfer_file_receiver.h"
#include "redclaw_wire.pb.h"
#include <limits>
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace redclaw::workspace {
namespace {
bool fail(std::string* error, const char* value) { if (error) *error = value; return false; }
bool native_name(std::string_view name) {
#ifdef _WIN32
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0);
    return count > 0 && count < MAX_PATH;
#else
    return !name.empty();
#endif
}
bool identity(const std::filesystem::path& path, ClipboardSourceIdentity& out) {
#ifdef _WIN32
    const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(handle, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
    CloseHandle(handle);
    if (!ok) return false;
    out = {path, info.dwVolumeSerialNumber, (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
        (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
        (std::uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime,
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0};
    return true;
#else
    (void)path; (void)out; return false;
#endif
}
}
std::string ClipboardDescription::serialize() const {
    protocol::wire::ClipboardDescriptionV1 value;
    value.set_schema_version(1); value.set_roots(roots);
    for (const auto& entry : entries) {
        auto* item = value.add_entries(); item->set_relative_path(entry.relative_path);
        item->set_staged_path(entry.staged_path); item->set_size(entry.size); item->set_directory(entry.directory);
    }
    if (value.ByteSizeLong() > kMaxClipboardDescriptionBytes) return {};
    return value.SerializeAsString();
}
std::shared_ptr<ClipboardDescription> ClipboardDescription::parse(std::string_view bytes, std::string* error) {
    const auto reject = [&](const char* code) { fail(error, code); return std::shared_ptr<ClipboardDescription>{}; };
    protocol::wire::ClipboardDescriptionV1 value;
    if (bytes.size() > kMaxClipboardDescriptionBytes || !value.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))
        || value.schema_version() != 1) return reject("clipboard_description_invalid");
    if (!value.roots() || value.roots() > 1024 || value.entries_size() > kMaxClipboardDescriptionEntries)
        return reject("clipboard_description_too_large");
    auto result = std::make_shared<ClipboardDescription>(); result->roots = value.roots();
    std::unordered_set<std::string> paths;
    for (const auto& item : value.entries()) {
        std::filesystem::path checked;
        const auto index_end = item.staged_path().find('/', 6);
        if (!native_name(item.relative_path()) || !validate_transfer_relative_path(item.relative_path(), &checked)
            || !validate_transfer_relative_path(item.staged_path(), &checked)
            || !item.staged_path().starts_with("files/")
            || index_end == std::string::npos || item.staged_path().substr(index_end + 1) != item.relative_path()
            || !paths.insert(item.staged_path()).second
            || (item.directory() && item.size())
            || item.size() > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
            || item.size() > (std::numeric_limits<std::uint64_t>::max)() - result->totals.bytes)
            return reject("clipboard_description_entry_invalid");
        result->entries.push_back({item.relative_path(), item.staged_path(), item.size(), item.directory()});
        ++result->totals.entries; if (!item.directory()) ++result->totals.files;
        result->totals.bytes += item.size();
    }
    if (result->entries.empty()) return reject("clipboard_description_empty");
    if (error) error->clear(); return result;
}
bool describe_clipboard_files(std::span<const ClipboardFileSelection> files, const std::filesystem::path& spool,
    const std::atomic_bool& cancelled, ClipboardDescription& out, std::string* error) {
    if (files.empty() || files.size() > 1024) return fail(error, "clipboard_file_roots_invalid");
    TransferSourceManifest manifest;
    if (!manifest.begin_scan(spool, error)) return false;
    for (const auto& file : files) {
        if (!manifest.add_source(file.source, [&](const auto& totals) {
            return !cancelled.load() && totals.entries <= kMaxClipboardDescriptionEntries;
        }, error, file.relative_root)) return false;
    }
    if (!manifest.finish_scan(error)) return false;
    out.roots = static_cast<std::uint32_t>(files.size()); out.totals = manifest.totals();
    TransferSourceEntry entry;
    for (;;) {
        if (cancelled.load()) return fail(error, "transfer_cancelled");
        const auto read = manifest.next(&entry, error);
        if (read == TransferManifestRead::kFailed) return false;
        if (read == TransferManifestRead::kEnd) break;
        const auto index_end = entry.relative_path.find('/', 6);
        if (!entry.relative_path.starts_with("files/") || index_end == std::string::npos)
            return fail(error, "clipboard_description_path_invalid");
        if (!native_name(entry.relative_path.substr(index_end + 1))) return fail(error, "clipboard_native_name_too_long");
        ClipboardSourceIdentity before;
        if (!identity(entry.source, before) || before.directory != entry.directory
            || (!entry.directory && before.size != entry.size)) return fail(error, "clipboard_source_changed");
        out.entries.push_back({entry.relative_path.substr(index_end + 1), entry.relative_path, entry.size, entry.directory});
        out.identities.push_back(std::move(before));
    }
    return !out.serialize().empty() || fail(error, "clipboard_description_too_large");
}
bool ClipboardDescription::sources_unchanged(const std::atomic_bool& cancelled, std::string* error) const {
    if (identities.empty() || identities.size() != entries.size()) return fail(error, "clipboard_source_identity_unavailable");
    for (const auto& before : identities) {
        ClipboardSourceIdentity now;
        if (cancelled.load() || !identity(before.path, now) || before.volume != now.volume || before.file != now.file
            || before.size != now.size || before.written != now.written || before.directory != now.directory)
            return fail(error, "clipboard_source_changed");
    }
    return true;
}
}
