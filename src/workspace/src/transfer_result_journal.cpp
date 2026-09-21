#include "redclaw/workspace/transfer_result_journal.h"
#include "redclaw/workspace/transfer_file_receiver.h"
#include "redclaw_wire.pb.h"
#include <algorithm>
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
constexpr std::array<char, 8> kMagic{'R', 'C', 'T', 'R', 1, 0, 0, 0};
constexpr std::uint32_t kMaxRecordBytes = 64U * 1024U;
bool fail(std::string* error, const char* code) { if (error) *error = code; return false; }
bool valid(const TransferResultEntry& entry) {
    std::filesystem::path checked;
    return entry.relative_path.size() <= 32760 && (!entry.directory || !entry.size)
        && validate_transfer_relative_path(entry.relative_path, &checked, nullptr);
}
#ifdef _WIN32
struct File {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~File() { close(); }
    void close() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
};
bool write(HANDLE file, const void* bytes, DWORD size) {
    DWORD written = 0;
    return WriteFile(file, bytes, size, &written, nullptr) && written == size;
}
bool read_at(HANDLE file, std::uint64_t offset, void* bytes, DWORD size) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) return false;
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD read = 0;
    return SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
        && ReadFile(file, bytes, size, &read, nullptr) && read == size;
}
std::array<unsigned char, 4> encode_length(std::uint32_t length) {
    return {static_cast<unsigned char>(length), static_cast<unsigned char>(length >> 8),
        static_cast<unsigned char>(length >> 16), static_cast<unsigned char>(length >> 24)};
}
std::uint32_t decode_length(const std::array<unsigned char, 4>& bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}
#endif
}
struct TransferResultJournal::Impl {
    std::filesystem::path path;
#ifdef _WIN32
    File file;
#endif
};
TransferResultJournal::TransferResultJournal() : impl_(std::make_unique<Impl>()) {}
TransferResultJournal::~TransferResultJournal() = default;
const std::filesystem::path& TransferResultJournal::path() const { return impl_->path; }
bool TransferResultJournal::begin(const std::filesystem::path& directory, std::string* error) {
#ifdef _WIN32
    if (!impl_->path.empty() || !directory.is_absolute()) return fail(error, "transfer_results_invalid_directory");
    std::error_code ec;
    const auto root = std::filesystem::canonical(directory, ec);
    if (ec) return fail(error, "transfer_results_directory_unavailable");
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) return fail(error, "transfer_identity_unavailable");
    std::string name = "transfer-results-";
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : random) { name += hex[byte >> 4]; name += hex[byte & 15]; }
    const auto path = root / (name + ".rctr");
    impl_->file.value = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (impl_->file.value == INVALID_HANDLE_VALUE) return fail(error, "transfer_results_create_failed");
    impl_->path = path;
    if (!write(impl_->file.value, kMagic.data(), static_cast<DWORD>(kMagic.size()))) {
        finish(); return fail(error, "transfer_results_write_failed");
    }
    if (error) error->clear();
    return true;
#else
    (void)directory; return fail(error, "transfer_results_unsupported");
#endif
}
bool TransferResultJournal::append(const TransferResultEntry& entry, std::string* error) {
    if (!valid(entry)) return fail(error, "transfer_results_invalid_entry");
#ifdef _WIN32
    protocol::wire::TransferResultRecordV1 record;
    record.set_schema_version(1); record.set_relative_path(entry.relative_path);
    record.set_size(entry.size); record.set_directory(entry.directory); record.set_skipped(entry.skipped);
    std::string bytes;
    if (record.ByteSizeLong() > kMaxRecordBytes || !record.SerializeToString(&bytes))
        return fail(error, "transfer_results_encoding_failed");
    const auto length = static_cast<std::uint32_t>(bytes.size());
    const auto marker = encode_length(length);
    if (!write(impl_->file.value, marker.data(), 4) || !write(impl_->file.value, bytes.data(), length)
        || !write(impl_->file.value, marker.data(), 4)) return fail(error, "transfer_results_write_failed");
    if (error) error->clear();
    return true;
#else
    return fail(error, "transfer_results_unsupported");
#endif
}
void TransferResultJournal::finish() {
#ifdef _WIN32
    impl_->file.close();
#endif
}
TransferResultPage read_transfer_result_page(const std::filesystem::path& path, std::uint64_t cursor, bool previous) {
    TransferResultPage result;
#ifdef _WIN32
    File file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION information{};
    if (file.value == INVALID_HANDLE_VALUE || GetFileType(file.value) != FILE_TYPE_DISK
        || !GetFileInformationByHandle(file.value, &information)
        || (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        result.error = "transfer_results_open_failed"; return result;
    }
    const auto size = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
    std::array<char, 8> magic{};
    if (!read_at(file.value, 0, magic.data(), static_cast<DWORD>(magic.size())) || magic != kMagic) {
        result.error = "transfer_results_version_or_header_invalid"; return result;
    }
    auto position = cursor ? cursor : kMagic.size();
    if (position < kMagic.size() || position > size) { result.error = "transfer_results_invalid_cursor"; return result; }
    result.begin = result.end = position;
    std::size_t page_bytes = 0;
    for (std::size_t count = 0; count < kTransferResultPageEntries; ++count) {
        if ((!previous && position == size) || (previous && position == kMagic.size())) break;
        std::array<unsigned char, 4> marker{}, tail{};
        if ((previous && position < kMagic.size() + 8)
            || (!previous && size - position < 8)
            || !read_at(file.value, previous ? position - 4 : position, marker.data(), 4)) {
            result.error = "transfer_results_truncated"; break;
        }
        const auto length = decode_length(marker);
        if (!result.entries.empty() && page_bytes + length > 32768) break;
        const auto available = previous ? position - kMagic.size() : size - position;
        if (!length || length > kMaxRecordBytes || static_cast<std::uint64_t>(length) + 8 > available) {
            result.error = "transfer_results_invalid_length"; break;
        }
        const auto begin = previous ? position - length - 8 : position;
        std::string bytes(length, '\0');
        if (!read_at(file.value, begin + 4, bytes.data(), length)
            || !read_at(file.value, previous ? begin : begin + 4 + length, tail.data(), 4) || marker != tail) {
            result.error = "transfer_results_record_incomplete"; break;
        }
        protocol::wire::TransferResultRecordV1 record;
        if (!record.ParseFromString(bytes) || record.schema_version() != 1) {
            result.error = "transfer_results_record_invalid"; break;
        }
        TransferResultEntry entry{record.relative_path(), record.size(), record.directory(), record.skipped()};
        if (!valid(entry)) { result.error = "transfer_results_invalid_entry"; break; }
        result.entries.push_back(std::move(entry));
        page_bytes += length;
        position = previous ? begin : begin + length + 8;
        if (previous) result.begin = position; else result.end = position;
    }
    if (previous) std::reverse(result.entries.begin(), result.entries.end());
    result.has_previous = result.begin > kMagic.size(); result.has_next = result.end < size;
#else
    (void)path; (void)cursor; (void)previous; result.error = "transfer_results_unsupported";
#endif
    return result;
}
}
