#include "redclaw/workspace/clipboard_payload.h"
#include "redclaw/workspace/transfer_file_receiver.h"
#include "redclaw/workspace/transfer_file_source.h"
#include "redclaw_wire.pb.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <set>
#include <utility>
#include <vector>
#include <openssl/evp.h>
#include <openssl/rand.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Shellapi.h>
#include <ShlObj.h>
#endif

namespace redclaw::workspace {
namespace {
constexpr std::size_t kChunk = 64U * 1024U;
bool fail(std::string* error, const char* code) { if (error) *error = code; return false; }
std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string(); return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::string format_file(std::uint32_t kind) { return "format-" + std::to_string(kind) + ".bin"; }
bool valid_hash(std::string_view hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
}
bool write_snapshot_file(TransferFileReceiver& writer, std::string name, std::span<const std::uint8_t> bytes,
    const std::atomic_bool& cancelled, std::string* hash, std::string* error) {
    const auto opened = writer.begin_file(name, bytes.size(), TransferConflict::kKeepBoth);
    if (opened.result != TransferEntryResult::kReceiving) return fail(error, "clipboard_snapshot_file_create_failed");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) return fail(error, "clipboard_hash_unavailable");
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (cancelled.load()) return fail(error, "clipboard_cancelled");
        const auto chunk = bytes.subspan(offset, std::min(kChunk, bytes.size() - offset));
        if (!writer.write(offset, chunk, error)) return false;
        if (EVP_DigestUpdate(digest.get(), chunk.data(), chunk.size()) != 1) return fail(error, "clipboard_hash_failed");
        offset += chunk.size();
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> result{}; unsigned length = 0;
    if (EVP_DigestFinal_ex(digest.get(), result.data(), &length) != 1 || length != 32) return fail(error, "clipboard_hash_failed");
    constexpr char hex[] = "0123456789abcdef";
    hash->clear(); for (unsigned i = 0; i < length; ++i) { *hash += hex[result[i] >> 4]; *hash += hex[result[i] & 15]; }
    const auto committed = writer.commit(*hash);
    return committed.result == TransferEntryResult::kCommitted || fail(error, "clipboard_snapshot_commit_failed");
}
#ifdef _WIN32
class ClipboardOpen {
public:
    explicit ClipboardOpen(HWND owner) : opened(OpenClipboard(owner) != FALSE) {}
    ~ClipboardOpen() { if (opened) CloseClipboard(); }
    bool opened;
};
class GlobalView {
public:
    explicit GlobalView(HGLOBAL handle) : handle_(handle), bytes(static_cast<const std::uint8_t*>(GlobalLock(handle))) {}
    ~GlobalView() { if (bytes) GlobalUnlock(handle_); }
private:
    HGLOBAL handle_;
public:
    const std::uint8_t* bytes;
};
UINT native_format(std::uint32_t kind) {
    switch (static_cast<ClipboardPayloadFormat>(kind)) {
    case ClipboardPayloadFormat::kUnicodeText: return CF_UNICODETEXT;
    case ClipboardPayloadFormat::kHtml: return RegisterClipboardFormatW(L"HTML Format");
    case ClipboardPayloadFormat::kRtf: return RegisterClipboardFormatW(L"Rich Text Format");
    case ClipboardPayloadFormat::kPng: return RegisterClipboardFormatW(L"PNG");
    case ClipboardPayloadFormat::kDibV5: return CF_DIBV5;
    case ClipboardPayloadFormat::kDib: return CF_DIB;
    }
    return 0;
}
struct ClipboardObject {
    UINT format = 0; HGLOBAL memory = nullptr;
    ClipboardObject(UINT type, std::size_t size) : format(type), memory(GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size)) {}
    ClipboardObject(ClipboardObject&& other) noexcept : format(other.format), memory(std::exchange(other.memory, nullptr)) {}
    ~ClipboardObject() { if (memory) GlobalFree(memory); }
    ClipboardObject(const ClipboardObject&) = delete;
    ClipboardObject& operator=(const ClipboardObject&) = delete;
};
#endif
}
struct ClipboardSnapshot::Impl {
    std::filesystem::path root, metadata;
    std::vector<ClipboardFileSelection> files;
    ~Impl() {
        // Delete only the fixed files we created. Never recursively remove a
        // tree that may have acquired unrelated content while being used.
        if (root.empty()) return;
        std::error_code ec;
#ifdef _WIN32
        if ((GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
            || (GetFileAttributesW(metadata.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)) return;
#endif
        if (std::filesystem::is_symlink(root, ec) || std::filesystem::is_symlink(metadata, ec)) return;
        for (std::uint32_t kind = 1; kind <= 6; ++kind) std::filesystem::remove(metadata / format_file(kind), ec);
        std::filesystem::remove(metadata / "manifest.pb", ec);
        std::filesystem::remove(metadata, ec); std::filesystem::remove(root, ec);
    }
};
ClipboardSnapshot::ClipboardSnapshot() : impl_(std::make_unique<Impl>()) {}
ClipboardSnapshot::~ClipboardSnapshot() = default;
const std::filesystem::path& ClipboardSnapshot::metadata_directory() const { return impl_->metadata; }
std::span<const ClipboardFileSelection> ClipboardSnapshot::files() const { return impl_->files; }
bool ClipboardSnapshot::capture(const std::filesystem::path& spool, std::uint32_t expected_sequence,
    const std::atomic_bool& cancelled, std::string* error) {
#ifdef _WIN32
    if (!impl_->root.empty() || cancelled.load() || !spool.is_absolute()) return fail(error, "clipboard_snapshot_invalid_start");
    std::error_code ec; const auto parent = std::filesystem::canonical(spool, ec);
    if (ec) return fail(error, "clipboard_spool_unavailable");
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) return fail(error, "clipboard_identity_unavailable");
    std::string name = "redclaw-clipboard-"; constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : random) { name += hex[byte >> 4]; name += hex[byte & 15]; }
    const auto root = parent / name;
    if (!CreateDirectoryW(root.c_str(), nullptr)) return fail(error, "clipboard_snapshot_create_failed");
    impl_->root = root; impl_->metadata = root / "metadata";
    if (!CreateDirectoryW(impl_->metadata.c_str(), nullptr)) return fail(error, "clipboard_snapshot_create_failed");
    TransferFileReceiver writer;
    if (!writer.begin_batch(impl_->metadata, name, error)) return false;
    protocol::wire::ClipboardPayloadManifestV1 manifest; manifest.set_schema_version(1);
    {
        ClipboardOpen clipboard(nullptr);
        if (!clipboard.opened) return fail(error, "clipboard_busy");
        const auto sequence = GetClipboardSequenceNumber();
        if (!sequence || (expected_sequence && sequence != expected_sequence)) return fail(error, "clipboard_changed_before_snapshot");
        // The open clipboard excludes another writer. Delayed rendering by its
        // existing owner can legitimately advance the sequence while reading.
        for (std::uint32_t kind = 1; kind <= 6; ++kind) {
            if (cancelled.load()) return fail(error, "clipboard_cancelled");
            if (kind == static_cast<std::uint32_t>(ClipboardPayloadFormat::kDib) && IsClipboardFormatAvailable(CF_DIBV5)) continue;
            const auto format = native_format(kind);
            if (!format || !IsClipboardFormatAvailable(format)) continue;
            const auto handle = static_cast<HGLOBAL>(GetClipboardData(format));
            if (!handle) return fail(error, "clipboard_format_unavailable");
            const auto allocated = GlobalSize(handle); GlobalView view(handle);
            if (!allocated || !view.bytes) return fail(error, "clipboard_format_invalid");
            std::size_t size = allocated;
            if (format == CF_UNICODETEXT) {
                if (size % sizeof(wchar_t)) return fail(error, "clipboard_text_invalid");
                const auto* text = reinterpret_cast<const wchar_t*>(view.bytes);
                const auto count = size / sizeof(wchar_t);
                const auto end = std::find(text, text + count, L'\0');
                if (end == text + count) return fail(error, "clipboard_text_unterminated");
                size = static_cast<std::size_t>(end - text + 1) * sizeof(wchar_t);
            } else if (kind == static_cast<std::uint32_t>(ClipboardPayloadFormat::kHtml)) {
                const auto* end = static_cast<const std::uint8_t*>(std::memchr(view.bytes, 0, size));
                if (end) size = static_cast<std::size_t>(end - view.bytes) + 1;
            }
            std::string hash;
            if (!write_snapshot_file(writer, format_file(kind), {view.bytes, size}, cancelled, &hash, error)) return false;
            auto* description = manifest.add_formats(); description->set_kind(kind); description->set_size(size); description->set_sha256(hash);
        }
        if (IsClipboardFormatAvailable(CF_HDROP)) {
            const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
            if (!drop) return fail(error, "clipboard_file_list_unavailable");
            const auto count = DragQueryFileW(drop, 0xffffffffU, nullptr, 0);
            for (UINT index = 0; index < count; ++index) {
                if (cancelled.load()) return fail(error, "clipboard_cancelled");
                const auto size = DragQueryFileW(drop, index, nullptr, 0);
                if (!size || size > 32760) return fail(error, "clipboard_file_path_invalid");
                std::wstring path(size + 1, L'\0');
                if (DragQueryFileW(drop, index, path.data(), size + 1) != size) return fail(error, "clipboard_file_list_changed");
                path.resize(size); std::filesystem::path source(path);
                const auto relative = "files/" + std::to_string(index) + "/" + utf8(source.filename());
                std::filesystem::path checked;
                if (!source.is_absolute() || !validate_transfer_relative_path(relative, &checked, error)) return fail(error, "clipboard_file_path_invalid");
                impl_->files.push_back({std::move(source), relative});
            }
            manifest.set_file_roots(count);
        }
    }
    if (!manifest.formats_size() && !manifest.file_roots()) return fail(error, "clipboard_format_unsupported");
    std::string metadata, hash;
    if (!manifest.SerializeToString(&metadata)) return fail(error, "clipboard_manifest_encoding_failed");
    if (!write_snapshot_file(writer, "manifest.pb", {reinterpret_cast<const std::uint8_t*>(metadata.data()), metadata.size()}, cancelled, &hash, error)
        || !writer.end_batch(error)) return false;
    if (error) error->clear(); return true;
#else
    (void)spool; (void)expected_sequence; (void)cancelled; return fail(error, "clipboard_platform_unsupported");
#endif
}
struct PreparedClipboardPayload::Impl {
    bool ready = false, used = false;
#ifdef _WIN32
    std::vector<ClipboardObject> objects;
#endif
};
PreparedClipboardPayload::PreparedClipboardPayload() : impl_(std::make_unique<Impl>()) {}
PreparedClipboardPayload::~PreparedClipboardPayload() = default;
bool PreparedClipboardPayload::load(const std::filesystem::path& directory, const std::atomic_bool& cancelled, std::string* error) {
#ifdef _WIN32
    if (impl_->ready || impl_->used || !impl_->objects.empty()) return fail(error, "clipboard_payload_already_loaded");
    TransferFileSource source;
    if (!source.open(directory / "metadata/manifest.pb", error)) return false;
    if (source.size() > kChunk) return fail(error, "clipboard_manifest_too_large");
    std::string bytes;
    for (;;) {
        if (cancelled.load()) return fail(error, "clipboard_cancelled");
        const auto read = source.read();
        if (read.state == TransferReadState::kFailed) return fail(error, "clipboard_manifest_read_failed");
        if (read.state == TransferReadState::kComplete) break;
        bytes.append(reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size());
    }
    source.close();
    protocol::wire::ClipboardPayloadManifestV1 manifest;
    if (!manifest.ParseFromString(bytes) || manifest.schema_version() != 1) return fail(error, "clipboard_manifest_version_or_format_invalid");
    if (manifest.formats_size() > 6 || (!manifest.formats_size() && !manifest.file_roots())) return fail(error, "clipboard_manifest_invalid");
    std::set<std::uint32_t> kinds;
    for (const auto& format : manifest.formats()) {
        const auto native = native_format(format.kind());
        if (!native || !kinds.insert(format.kind()).second || !valid_hash(format.sha256()) || !format.size()
            || format.size() > std::numeric_limits<std::size_t>::max()) return fail(error, "clipboard_manifest_invalid");
        if (!source.open(directory / "metadata" / format_file(format.kind()), error)) return false;
        if (source.size() != format.size()) return fail(error, "clipboard_payload_size_mismatch");
        impl_->objects.emplace_back(native, static_cast<std::size_t>(format.size()));
        auto& object = impl_->objects.back();
        if (!object.memory) return fail(error, "clipboard_allocation_failed");
        auto* memory = static_cast<std::uint8_t*>(GlobalLock(object.memory));
        if (!memory) return fail(error, "clipboard_allocation_failed");
        bool ok = true;
        for (;;) {
            if (cancelled.load()) { ok = fail(error, "clipboard_cancelled"); break; }
            const auto read = source.read();
            if (read.state == TransferReadState::kFailed) { ok = fail(error, "clipboard_payload_read_failed"); break; }
            if (read.state == TransferReadState::kComplete) {
                if (read.sha256 != format.sha256()) ok = fail(error, "clipboard_payload_checksum_mismatch");
                break;
            }
            if (read.offset > format.size() || read.bytes.size() > format.size() - read.offset) { ok = fail(error, "clipboard_payload_size_mismatch"); break; }
            std::memcpy(memory + static_cast<std::size_t>(read.offset), read.bytes.data(), read.bytes.size());
        }
        if (ok && native == CF_UNICODETEXT && (format.size() % sizeof(wchar_t) || format.size() < sizeof(wchar_t)
            || reinterpret_cast<wchar_t*>(memory)[format.size() / sizeof(wchar_t) - 1] != L'\0')) ok = fail(error, "clipboard_text_invalid");
        GlobalUnlock(object.memory); source.close(); if (!ok) return false;
    }
    if (manifest.file_roots()) {
        std::vector<std::wstring> paths;
        std::size_t total = sizeof(DROPFILES) + sizeof(wchar_t);
        for (std::uint32_t index = 0; index < manifest.file_roots(); ++index) {
            if (cancelled.load()) return fail(error, "clipboard_cancelled");
            const auto root = directory / "files" / std::to_string(index);
            const auto attributes = GetFileAttributesW(root.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                return fail(error, "clipboard_received_files_invalid");
            std::error_code ec; std::filesystem::directory_iterator it(root, ec), end;
            if (ec || it == end) return fail(error, "clipboard_received_files_missing");
            const auto selected = it->path(); it.increment(ec);
            const auto selected_attributes = GetFileAttributesW(selected.c_str());
            if (ec || it != end || selected_attributes == INVALID_FILE_ATTRIBUTES || (selected_attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                return fail(error, "clipboard_received_files_invalid");
            auto path = selected.wstring();
            if (path.size() > 32760 || (path.size() + 1) > (std::numeric_limits<std::size_t>::max() - total) / sizeof(wchar_t))
                return fail(error, "clipboard_file_list_too_large");
            total += (path.size() + 1) * sizeof(wchar_t); paths.push_back(std::move(path));
        }
        impl_->objects.emplace_back(CF_HDROP, total); auto& object = impl_->objects.back();
        if (!object.memory) return fail(error, "clipboard_allocation_failed");
        auto* memory = static_cast<std::uint8_t*>(GlobalLock(object.memory));
        if (!memory) return fail(error, "clipboard_allocation_failed");
        auto* drop = reinterpret_cast<DROPFILES*>(memory); drop->pFiles = sizeof(DROPFILES); drop->fWide = TRUE;
        auto* position = reinterpret_cast<wchar_t*>(memory + sizeof(DROPFILES));
        for (const auto& path : paths) { std::memcpy(position, path.c_str(), (path.size() + 1) * sizeof(wchar_t)); position += path.size() + 1; }
        GlobalUnlock(object.memory);
        const auto effect = RegisterClipboardFormatW(L"Preferred DropEffect");
        impl_->objects.emplace_back(effect, sizeof(DWORD)); auto& copy = impl_->objects.back();
        auto* effect_memory = copy.memory ? static_cast<DWORD*>(GlobalLock(copy.memory)) : nullptr;
        if (!effect || !effect_memory) return fail(error, "clipboard_allocation_failed");
        *effect_memory = DROPEFFECT_COPY; GlobalUnlock(copy.memory);
    }
    impl_->ready = true; if (error) error->clear(); return true;
#else
    (void)directory; (void)cancelled; return fail(error, "clipboard_platform_unsupported");
#endif
}
bool PreparedClipboardPayload::publish(const std::function<bool()>& final_guard, std::string* error) {
#ifdef _WIN32
    if (!impl_->ready || impl_->used || !final_guard) return fail(error, "clipboard_payload_not_ready");
    impl_->used = true;
    const auto owner = CreateWindowExW(0, L"STATIC", L"RedClaw Clipboard", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) return fail(error, "clipboard_owner_unavailable");
    bool result = true;
    {
        ClipboardOpen clipboard(owner);
        if (!clipboard.opened) result = fail(error, "clipboard_busy");
        else if (!final_guard()) result = fail(error, "clipboard_paste_invalidated");
        else if (!EmptyClipboard()) result = fail(error, "clipboard_empty_failed");
        else for (auto& object : impl_->objects) {
            if (!SetClipboardData(object.format, object.memory)) { result = fail(error, "clipboard_publication_failed"); break; }
            object.memory = nullptr; // ownership passed to Windows
        }
    }
    DestroyWindow(owner);
    if (result && error) error->clear(); return result;
#else
    (void)final_guard; return fail(error, "clipboard_platform_unsupported");
#endif
}
}
