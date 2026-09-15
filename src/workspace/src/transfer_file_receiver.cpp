#include "redclaw/workspace/transfer_file_receiver.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>
#include <utility>
#include <openssl/evp.h>
#include <openssl/rand.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace redclaw::workspace {
namespace {
bool fail(std::string* error, std::string code) { if (error) *error = std::move(code); return false; }
std::string random_suffix() {
    std::array<unsigned char, 12> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
bool valid_operation(std::string_view id) {
    if (id.empty() || id.size() > 128) return false;
    return std::all_of(id.begin(), id.end(), [](char ch) { return (ch >= 'a' && ch <= 'z')
        || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'; });
}
#ifdef _WIN32
class Handle final {
public:
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { reset(); value = std::exchange(other.value, INVALID_HANDLE_VALUE); }
        return *this;
    }
    void reset() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
    bool valid() const { return value != INVALID_HANDLE_VALUE; }
};
Handle lock_directory(const std::filesystem::path& path, std::string* error) {
    // Deny write/delete sharing on the directory object while resolving its
    // children. This excludes replacement/reparse mutations of an ancestor.
    Handle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid()) { fail(error, "transfer_directory_open:" + std::to_string(GetLastError())); return {}; }
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle.value, FileAttributeTagInfo, &info, sizeof(info))
        || !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        fail(error, "transfer_directory_reparse_or_invalid"); return {};
    }
    return handle;
}
#endif
}
bool validate_transfer_relative_path(std::string_view text, std::filesystem::path* path, std::string* error) {
    if (!path || text.empty() || text.size() > 32760) return fail(error, "transfer_invalid_path");
    std::string component;
    const auto check = [&]() {
        if (component.empty() || component == "." || component == ".." || component.back() == '.' || component.back() == ' ') return false;
        auto base = component.substr(0, component.find('.'));
        std::transform(base.begin(), base.end(), base.begin(), [](unsigned char ch) { return ch >= 'a' && ch <= 'z' ? ch - ('a' - 'A') : ch; });
        const auto suffix = base.size() >= 3 ? std::string_view(base).substr(3) : std::string_view{};
        const bool numbered_device = (base.starts_with("COM") || base.starts_with("LPT"))
            && ((suffix.size() == 1 && suffix.front() >= '1' && suffix.front() <= '9')
                || suffix == "\xc2\xb9" || suffix == "\xc2\xb2" || suffix == "\xc2\xb3");
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL"
            || base == "CONIN$" || base == "CONOUT$" || numbered_device) return false;
        return true;
    };
    std::string normalized;
    for (unsigned char ch : text) {
        if (ch == '/' || ch == '\\') {
            if (!check()) return fail(error, "transfer_invalid_path");
            component.clear(); normalized += '/';
        } else {
            if (ch < 32 || ch == ':' || ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '?' || ch == '*') return fail(error, "transfer_invalid_path");
            component += static_cast<char>(ch); normalized += static_cast<char>(ch);
        }
    }
    if (!check()) return fail(error, "transfer_invalid_path");
    try { *path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(normalized.data()), normalized.size())); }
    catch (const std::exception&) { return fail(error, "transfer_invalid_path_encoding"); }
    if (path->is_absolute() || path->has_root_name() || path->has_root_directory()) return fail(error, "transfer_invalid_path");
    if (error) error->clear();
    return true;
}
struct TransferFileReceiver::Impl {
    std::filesystem::path root, relative, temporary;
    std::string operation;
    TransferConflict conflict = TransferConflict::kKeepBoth;
    std::uint64_t expected = 0, received = 0;
    EVP_MD_CTX* digest = nullptr;
#ifdef _WIN32
    Handle root_handle, file;
    std::vector<Handle> parents;
    bool ensure_parent(const std::filesystem::path& relative_directory, std::string* error) {
        parents.clear();
        auto current = root;
        for (const auto& component : relative_directory) {
            current /= component;
            if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                return fail(error, "transfer_directory_create:" + std::to_string(GetLastError()));
            auto handle = lock_directory(current, error);
            if (!handle.valid()) return false;
            parents.push_back(std::move(handle));
        }
        return true;
    }
#endif
};
TransferFileReceiver::TransferFileReceiver() : impl_(std::make_unique<Impl>()) {}
TransferFileReceiver::~TransferFileReceiver() { cancel_file(); }
bool TransferFileReceiver::begin_batch(const std::filesystem::path& destination, std::string operation, std::string* error) {
    if (!cancel_file(error)) return false;
    impl_ = std::make_unique<Impl>();
    if (!destination.is_absolute() || !valid_operation(operation)) return fail(error, "transfer_invalid_batch");
#ifdef _WIN32
    std::error_code filesystem_error;
    const auto root = std::filesystem::canonical(destination, filesystem_error);
    if (filesystem_error) return fail(error, "transfer_destination_unavailable");
    auto handle = lock_directory(root, error);
    if (!handle.valid()) return false;
    impl_->root = root; impl_->operation = std::move(operation); impl_->root_handle = std::move(handle);
    if (error) error->clear();
    return true;
#else
    return fail(error, "transfer_receiver_unsupported");
#endif
}
bool TransferFileReceiver::directory(std::string_view relative, std::string* error) {
    std::filesystem::path path;
    if (impl_->root.empty() || receiving()) return fail(error, "transfer_receiver_busy_or_unavailable");
    if (!validate_transfer_relative_path(relative, &path, error)) return false;
#ifdef _WIN32
    const bool result = impl_->ensure_parent(path, error);
    impl_->parents.clear();
    return result;
#else
    return fail(error, "transfer_receiver_unsupported");
#endif
}
TransferFileResult TransferFileReceiver::begin_file(std::string_view relative, std::uint64_t size, TransferConflict conflict) {
    TransferFileResult result;
    if (impl_->root.empty() || receiving()) { result.error = "transfer_receiver_busy_or_unavailable"; return result; }
    if (!validate_transfer_relative_path(relative, &result.relative_path, &result.error)) return result;
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) { result.error = "transfer_invalid_size"; return result; }
#ifdef _WIN32
    if (!impl_->ensure_parent(result.relative_path.parent_path(), &result.error)) return result;
    const auto destination = impl_->root / result.relative_path;
    const auto attributes = GetFileAttributesW(destination.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        result.error = "transfer_destination_type_conflict"; impl_->parents.clear(); return result;
    }
    if (attributes != INVALID_FILE_ATTRIBUTES && conflict == TransferConflict::kSkip) {
        result.result = TransferEntryResult::kSkipped; impl_->parents.clear(); return result;
    }
    const auto random = random_suffix();
    if (random.empty()) { result.error = "transfer_identity_unavailable"; impl_->parents.clear(); return result; }
    impl_->temporary = destination.parent_path() / (L".redclaw-" + std::filesystem::path(impl_->operation + "-" + random).wstring() + L".partial");
    impl_->file = Handle(CreateFileW(impl_->temporary.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
        0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!impl_->file.valid()) { result.error = "transfer_temporary_create:" + std::to_string(GetLastError()); impl_->parents.clear(); return result; }
    impl_->digest = EVP_MD_CTX_new();
    if (!impl_->digest || EVP_DigestInit_ex(impl_->digest, EVP_sha256(), nullptr) != 1) {
        result.error = "transfer_hash_initialize"; cancel_file(); return result;
    }
    impl_->relative = result.relative_path; impl_->expected = size; impl_->received = 0; impl_->conflict = conflict;
    result.result = TransferEntryResult::kReceiving;
#else
    result.error = "transfer_receiver_unsupported";
#endif
    return result;
}
bool TransferFileReceiver::write(std::uint64_t offset, std::span<const std::uint8_t> bytes, std::string* error) {
    if (!receiving() || offset != impl_->received || bytes.empty() || bytes.size() > 64U * 1024U
        || bytes.size() > impl_->expected - impl_->received) return fail(error, "transfer_invalid_chunk");
#ifdef _WIN32
    DWORD written = 0;
    if (!WriteFile(impl_->file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size()) {
        const auto code = GetLastError(); cancel_file(); return fail(error, "transfer_write_failed:" + std::to_string(code));
    }
    if (EVP_DigestUpdate(impl_->digest, bytes.data(), bytes.size()) != 1) { cancel_file(); return fail(error, "transfer_hash_update"); }
    impl_->received += written;
    if (error) error->clear();
    return true;
#else
    return fail(error, "transfer_receiver_unsupported");
#endif
}
TransferFileResult TransferFileReceiver::commit(std::string_view expected_hash) {
    TransferFileResult result{TransferEntryResult::kFailed, impl_->relative, impl_->received, {}};
    if (!receiving()) { result.error = "transfer_no_pending_file"; return result; }
    const auto reject = [&](std::string reason) {
        result.error = std::move(reason);
        std::string cleanup_error;
        if (!cancel_file(&cleanup_error)) result.error = std::move(cleanup_error);
        return result;
    };
    if (impl_->received != impl_->expected) return reject("transfer_size_mismatch");
    if (expected_hash.size() != 64 || !std::all_of(expected_hash.begin(), expected_hash.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); })) return reject("transfer_invalid_sha256");
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned length = 0;
    if (EVP_DigestFinal_ex(impl_->digest, digest.data(), &length) != 1 || length != 32) return reject("transfer_hash_finalize");
    constexpr char hex[] = "0123456789abcdef";
    std::string actual;
    for (unsigned index = 0; index < length; ++index) { actual += hex[digest[index] >> 4]; actual += hex[digest[index] & 15]; }
    if (actual != expected_hash) return reject("transfer_sha256_mismatch");
#ifdef _WIN32
    if (!FlushFileBuffers(impl_->file.value)) return reject("transfer_flush_failed:" + std::to_string(GetLastError()));
    auto target = impl_->root / impl_->relative;
    // Keep-both commits never replace an entry, even if a same-name file appears
    // after scanning. A fresh suffix also bounds conflict retries.
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto name = target.wstring();
        std::vector<unsigned char> storage(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t), 0);
        auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        rename->ReplaceIfExists = impl_->conflict == TransferConflict::kOverwrite;
        rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
        std::memcpy(rename->FileName, name.data(), rename->FileNameLength);
        if (SetFileInformationByHandle(impl_->file.value, FileRenameInfo, rename, static_cast<DWORD>(storage.size()))) {
            result.result = TransferEntryResult::kCommitted;
            result.relative_path = target.lexically_relative(impl_->root);
            impl_->file.reset(); impl_->parents.clear();
            EVP_MD_CTX_free(impl_->digest); impl_->digest = nullptr;
            impl_->temporary.clear();
            return result;
        }
        const auto code = GetLastError();
        if (impl_->conflict == TransferConflict::kSkip && (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS)) {
            cancel_file(); result.result = TransferEntryResult::kSkipped; return result;
        }
        if (impl_->conflict != TransferConflict::kKeepBoth || (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS))
            return reject("transfer_commit_failed:" + std::to_string(code));
        const auto suffix = random_suffix();
        if (suffix.empty()) return reject("transfer_identity_unavailable");
        target = target.parent_path() / (impl_->relative.stem().wstring() + L" (" + std::filesystem::path(suffix).wstring() + L")" + impl_->relative.extension().wstring());
    }
    return reject("transfer_conflict_changed");
#else
    return reject("transfer_receiver_unsupported");
#endif
}
bool TransferFileReceiver::cancel_file(std::string* error) {
    bool cleaned = true;
#ifdef _WIN32
    if (impl_->file.valid()) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(impl_->file.value, FileDispositionInfo, &disposition, sizeof(disposition)))
            cleaned = fail(error, "transfer_cleanup_failed:" + std::to_string(GetLastError()));
        impl_->file.reset();
    }
    impl_->parents.clear();
#endif
    if (impl_->digest) { EVP_MD_CTX_free(impl_->digest); impl_->digest = nullptr; }
    impl_->temporary.clear(); impl_->relative.clear(); impl_->expected = impl_->received = 0;
    if (cleaned && error) error->clear();
    return cleaned;
}
bool TransferFileReceiver::end_batch(std::string* error) {
    const bool cleaned = cancel_file(error);
#ifdef _WIN32
    impl_->root_handle.reset();
#endif
    impl_->root.clear(); impl_->operation.clear();
    return cleaned;
}
bool TransferFileReceiver::receiving() const {
#ifdef _WIN32
    return impl_->file.valid();
#else
    return false;
#endif
}
std::uint64_t TransferFileReceiver::received_bytes() const { return impl_->received; }
}
