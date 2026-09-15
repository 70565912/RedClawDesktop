#include "redclaw/workspace/transfer_source_manifest.h"
#include "redclaw/workspace/transfer_file_receiver.h"
#include "redclaw_wire.pb.h"
#include <array>
#include <limits>
#include <openssl/rand.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace redclaw::workspace {
namespace {
constexpr std::uint32_t kMaxRecordBytes = 256U * 1024U;
bool fail(std::string* error, std::string code) { if (error) *error = std::move(code); return false; }
std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
}
struct TransferSourceManifest::Impl {
    TransferScanProgress totals;
    std::uint64_t read_entries = 0;
    bool started = false, ready = false;
    std::filesystem::path spool_path;
#ifdef _WIN32
    HANDLE spool = INVALID_HANDLE_VALUE;
    ~Impl() { close(); }
    void close() { if (spool != INVALID_HANDLE_VALUE) CloseHandle(spool); spool = INVALID_HANDLE_VALUE; ready = false; }
    bool write_record(const TransferSourceEntry& entry, std::string* error) {
        protocol::wire::TransferSourceRecordV1 record;
        record.set_schema_version(1); record.set_absolute_source(utf8(entry.source));
        record.set_relative_path(entry.relative_path); record.set_size(entry.size); record.set_directory(entry.directory);
        if (record.ByteSizeLong() > kMaxRecordBytes) return fail(error, "transfer_manifest_path_too_long");
        std::string bytes;
        if (!record.SerializeToString(&bytes)) return fail(error, "transfer_manifest_encoding");
        const auto length = static_cast<std::uint32_t>(bytes.size());
        const std::array<std::uint8_t, 4> header{static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8),
            static_cast<std::uint8_t>(length >> 16), static_cast<std::uint8_t>(length >> 24)};
        DWORD written = 0;
        if (!WriteFile(spool, header.data(), static_cast<DWORD>(header.size()), &written, nullptr) || written != header.size()
            || !WriteFile(spool, bytes.data(), length, &written, nullptr) || written != length)
            return fail(error, "transfer_manifest_write:" + std::to_string(GetLastError()));
        return true;
    }
#endif
};
TransferSourceManifest::TransferSourceManifest() : impl_(std::make_unique<Impl>()) {}
TransferSourceManifest::~TransferSourceManifest() = default;
bool TransferSourceManifest::scan(std::span<const std::filesystem::path> selected,
    const std::filesystem::path& spool_directory, const std::function<bool(const TransferScanProgress&)>& progress,
    std::string* error) {
    if (selected.empty()) return fail(error, "transfer_manifest_empty_selection");
    if (!begin_scan(spool_directory, error)) return false;
    for (const auto& source : selected) if (!add_source(source, progress, error)) return false;
    return finish_scan(error);
}
bool TransferSourceManifest::begin_scan(const std::filesystem::path& spool_directory, std::string* error) {
    if (impl_->started) return fail(error, "transfer_manifest_already_started");
    impl_->started = true;
#ifdef _WIN32
    const auto reject = [&](std::string code) { impl_->close(); return fail(error, std::move(code)); };
    if (cancelled_.load(std::memory_order_acquire)) return reject("transfer_cancelled");
    if (!spool_directory.is_absolute()) return reject("transfer_manifest_directory_not_absolute");
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) return reject("transfer_identity_unavailable");
    std::string name = ".redclaw-manifest-";
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : random) { name += hex[byte >> 4]; name += hex[byte & 15]; }
    std::error_code ec;
    const auto spool_root = std::filesystem::canonical(spool_directory, ec);
    if (ec) return reject("transfer_manifest_directory_unavailable");
    const auto path = spool_root / (name + ".tmp");
    impl_->spool_path = path;
    impl_->spool = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (impl_->spool == INVALID_HANDLE_VALUE) return reject("transfer_manifest_create:" + std::to_string(GetLastError()));
    if (error) error->clear();
    return true;
#else
    (void)spool_directory;
    return fail(error, "transfer_source_unsupported");
#endif
}
bool TransferSourceManifest::add_source(const std::filesystem::path& selected_path,
    const std::function<bool(const TransferScanProgress&)>& progress, std::string* error, std::string_view relative_root) {
#ifdef _WIN32
    const auto reject = [&](std::string code) { impl_->close(); return fail(error, std::move(code)); };
    if (!impl_->started || impl_->ready || impl_->spool == INVALID_HANDLE_VALUE)
        return reject("transfer_manifest_not_scanning");
    std::error_code ec;
    const auto cancelled = [&] { return cancelled_.load(std::memory_order_acquire); };
    const auto add = [&](const std::filesystem::path& source, const std::filesystem::path& relative) {
        if (cancelled()) return fail(error, "transfer_cancelled");
        // A selected tree can contain the local spool directory. Exclude only
        // this newly created, exclusively owned file, not any user content.
        if (CompareStringOrdinal(source.c_str(), -1, impl_->spool_path.c_str(), -1, TRUE) == CSTR_EQUAL) return true;
        const auto attributes = GetFileAttributesW(source.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) return fail(error, "transfer_scan_attributes:" + std::to_string(GetLastError()));
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return fail(error, "transfer_source_reparse_point");
        TransferSourceEntry entry{source, utf8(relative), 0, (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0};
        std::filesystem::path checked;
        if (!validate_transfer_relative_path(entry.relative_path, &checked, error)) return false;
        if (!entry.directory) {
            entry.size = std::filesystem::file_size(source, ec);
            if (ec) return fail(error, "transfer_scan_file_size");
            if (entry.size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                || entry.size > std::numeric_limits<std::uint64_t>::max() - impl_->totals.bytes)
                return fail(error, "transfer_scan_size_overflow");
        }
        if (impl_->totals.entries == std::numeric_limits<std::uint64_t>::max()) return fail(error, "transfer_scan_entry_overflow");
        if (!impl_->write_record(entry, error)) return false;
        ++impl_->totals.entries; if (!entry.directory) ++impl_->totals.files;
        impl_->totals.bytes += entry.size;
        if (progress && !progress(impl_->totals)) { request_cancel(); return fail(error, "transfer_cancelled"); }
        return true;
    };
    try {
        {
            if (cancelled()) return reject("transfer_cancelled");
            if (!selected_path.is_absolute()) return reject("transfer_source_path_not_absolute");
            auto root = selected_path.lexically_normal();
            if (root.filename().empty()) root = root.parent_path();
            const auto attributes = GetFileAttributesW(root.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) return reject("transfer_source_unavailable");
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return reject("transfer_source_reparse_point");
            const auto canonical_root = std::filesystem::canonical(root, ec);
            if (ec) return reject("transfer_source_unavailable");
            root = canonical_root;
            auto destination_root = root.filename();
            if (!relative_root.empty() && !validate_transfer_relative_path(relative_root, &destination_root, error))
                return reject("transfer_invalid_relative_root");
            if (!add(root, destination_root)) return reject(error ? *error : "transfer_scan_failed");
            if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) { if (error) error->clear(); return true; }
            // default options do not follow directory symlinks; encountering
            // any reparse entry is explicit failure, not an incomplete success.
            std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::none, ec), end;
            if (ec) return reject("transfer_scan_directory_unavailable");
            for (; it != end; it.increment(ec)) {
                if (ec) return reject("transfer_scan_directory_failed");
                if (!add(it->path(), destination_root / it->path().lexically_relative(root)))
                    return reject(error ? *error : "transfer_scan_failed");
            }
            if (ec) return reject("transfer_scan_directory_failed");
        }
    } catch (const std::filesystem::filesystem_error&) { return reject("transfer_scan_filesystem_error"); }
    if (error) error->clear();
    return true;
#else
    (void)selected_path; (void)progress; (void)relative_root;
    return fail(error, "transfer_source_unsupported");
#endif
}
bool TransferSourceManifest::finish_scan(std::string* error) {
#ifdef _WIN32
    const auto reject = [&](std::string code) { impl_->close(); return fail(error, std::move(code)); };
    if (!impl_->started || impl_->ready || impl_->spool == INVALID_HANDLE_VALUE)
        return reject("transfer_manifest_not_scanning");
    if (!impl_->totals.entries) return reject("transfer_manifest_empty_selection");
    const auto cancelled = [&] { return cancelled_.load(std::memory_order_acquire); };
    if (cancelled()) return reject("transfer_cancelled");
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(impl_->spool, beginning, nullptr, FILE_BEGIN)) return reject("transfer_manifest_rewind");
    impl_->ready = true;
    if (error) error->clear();
    return true;
#else
    return fail(error, "transfer_source_unsupported");
#endif
}
TransferManifestRead TransferSourceManifest::next(TransferSourceEntry* entry, std::string* error) {
#ifdef _WIN32
    const auto reject = [&](std::string code) {
        impl_->close(); fail(error, std::move(code)); return TransferManifestRead::kFailed;
    };
    if (cancelled_.load(std::memory_order_acquire)) return reject("transfer_cancelled");
    if (!entry || !impl_->ready) return reject("transfer_manifest_not_ready");
    if (impl_->read_entries == impl_->totals.entries) { if (error) error->clear(); return TransferManifestRead::kEnd; }
    std::array<std::uint8_t, 4> header{};
    DWORD count = 0;
    if (!ReadFile(impl_->spool, header.data(), static_cast<DWORD>(header.size()), &count, nullptr) || count != header.size())
        return reject("transfer_manifest_read_header");
    const std::uint32_t length = header[0] | (static_cast<std::uint32_t>(header[1]) << 8)
        | (static_cast<std::uint32_t>(header[2]) << 16) | (static_cast<std::uint32_t>(header[3]) << 24);
    if (!length || length > kMaxRecordBytes) return reject("transfer_manifest_invalid_record_size");
    std::string bytes(length, '\0');
    if (!ReadFile(impl_->spool, bytes.data(), length, &count, nullptr) || count != length)
        return reject("transfer_manifest_read_record");
    protocol::wire::TransferSourceRecordV1 record;
    if (!record.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) return reject("transfer_manifest_invalid_record");
    if (record.schema_version() != 1) return reject("protocol_version_incompatible");
    std::filesystem::path relative;
    if (!validate_transfer_relative_path(record.relative_path(), &relative, error)) return reject("transfer_manifest_invalid_path");
    try {
        entry->source = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(record.absolute_source().data()),
            record.absolute_source().size()));
    } catch (const std::exception&) { return reject("transfer_manifest_invalid_source_encoding"); }
    if (!entry->source.is_absolute()) return reject("transfer_manifest_invalid_source");
    entry->relative_path = record.relative_path(); entry->size = record.size(); entry->directory = record.directory();
    ++impl_->read_entries;
    if (error) error->clear();
    return TransferManifestRead::kEntry;
#else
    (void)entry; fail(error, "transfer_source_unsupported"); return TransferManifestRead::kFailed;
#endif
}
void TransferSourceManifest::request_cancel() noexcept { cancelled_.store(true, std::memory_order_release); }
void TransferSourceManifest::close() {
#ifdef _WIN32
    impl_->close();
#endif
}
TransferScanProgress TransferSourceManifest::totals() const { return impl_->totals; }
}
