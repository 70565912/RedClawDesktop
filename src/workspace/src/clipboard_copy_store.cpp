#include "redclaw/workspace/clipboard_copy_store.h"
#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>
#include <openssl/rand.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Shellapi.h>
#endif

namespace redclaw::workspace {
namespace {
constexpr char kReceipt[] = "RedClaw received clipboard batch v1\n";
bool fail(std::string* error, const char* reason) { if (error) *error = reason; return false; }
#ifdef _WIN32
struct CloseFile { void operator()(void* value) const { CloseHandle(value); } };
struct CloseSearch { void operator()(void* value) const { FindClose(value); } };
using File = std::unique_ptr<void, CloseFile>;
using Search = std::unique_ptr<void, CloseSearch>;
File open(const std::filesystem::path& path, DWORD access, DWORD creation = OPEN_EXISTING) {
    const auto value = CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr, creation,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    return File(value == INVALID_HANDLE_VALUE ? nullptr : value);
}
bool plain_directory(File& handle) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    return handle && GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &info, sizeof(info))
        && (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool pin_ancestors(const std::filesystem::path& path, std::vector<File>& handles, std::string* error) {
    if (!path.is_absolute() || path != path.lexically_normal()) return fail(error, "clipboard_copy_path_invalid");
    auto current = path.root_path();
    auto root = open(current, FILE_READ_ATTRIBUTES);
    if (!plain_directory(root)) return fail(error, "clipboard_copy_ancestor_unavailable");
    handles.push_back(std::move(root));
    for (const auto& part : path.relative_path()) {
        current /= part;
        auto handle = open(current, FILE_READ_ATTRIBUTES);
        if (!plain_directory(handle)) return fail(error, "clipboard_copy_ancestor_unavailable");
        handles.push_back(std::move(handle));
    }
    return true;
}
bool remove_handle(File& handle, std::string* error) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
        return fail(error, "clipboard_copy_file_in_use_or_denied");
    handle.reset(); return true;
}
bool marker_matches(const std::filesystem::path& path, std::string_view id) {
    auto marker = open(path / L".redclaw-clipboard-batch", GENERIC_READ);
    if (!marker) return false;
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(marker.get(), FileAttributeTagInfo, &info, sizeof(info))
        || (info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) return false;
    const auto expected = std::string(kReceipt) + std::string(id);
    std::array<char, sizeof(kReceipt) + 32> bytes{}; DWORD size = 0;
    return ReadFile(marker.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &size, nullptr)
        && size == expected.size() && std::string_view(bytes.data(), size) == expected;
}
bool write_marker(const std::filesystem::path& batch, std::string_view id, std::string* error) {
    auto marker = open(batch / L".redclaw-clipboard-batch", GENERIC_WRITE, CREATE_NEW);
    if (!marker) return fail(error, "clipboard_copy_receipt_create_failed");
    const auto bytes = std::string(kReceipt) + std::string(id); DWORD written = 0;
    if (!WriteFile(marker.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        || written != bytes.size() || !FlushFileBuffers(marker.get())) return fail(error, "clipboard_copy_receipt_write_failed");
    return true;
}
struct DirectoryFrame {
    std::filesystem::path path;
    File handle;
    Search search;
    WIN32_FIND_DATAW entry{};
    bool started = false;
};
#endif
}
ClipboardCopyStore::ClipboardCopyStore(std::filesystem::path spool)
    : directory_(std::move(spool) / "redclaw-clipboard-copies-v1") {}
bool ClipboardCopyStore::valid_id(std::string_view id) {
    return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}
std::string ClipboardCopyStore::new_id() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result; result.reserve(32);
    for (const auto byte : bytes) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
std::filesystem::path ClipboardCopyStore::batch_directory(std::string_view id) const {
    return valid_id(id) ? directory_ / std::string(id) : std::filesystem::path{};
}
bool ClipboardCopyStore::create_batch(std::string_view id, std::string* error) {
    if (!valid_id(id)) return fail(error, "clipboard_copy_id_invalid");
#ifdef _WIN32
    std::vector<File> parents;
    if (!pin_ancestors(directory_.parent_path(), parents, error)) return false;
    if (!CreateDirectoryW(directory_.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return fail(error, "clipboard_copy_container_create_failed");
    auto container = open(directory_, FILE_READ_ATTRIBUTES);
    if (!plain_directory(container)) return fail(error, "clipboard_copy_container_invalid");
    const auto batch = batch_directory(id);
    if (!CreateDirectoryW(batch.c_str(), nullptr)) return fail(error, "clipboard_copy_batch_already_exists_or_denied");
    auto batch_handle = open(batch, FILE_READ_ATTRIBUTES);
    if (!plain_directory(batch_handle)) return fail(error, "clipboard_copy_batch_invalid");
    if (!write_marker(batch, id, error)) return false;
    if (error) error->clear(); return true;
#else
    return fail(error, "clipboard_platform_unsupported");
#endif
}
bool ClipboardCopyStore::contains_batch(std::string_view id) const {
#ifdef _WIN32
    if (!valid_id(id)) return false;
    std::vector<File> parents;
    if (!pin_ancestors(directory_, parents, nullptr)) return false;
    auto batch = open(batch_directory(id), FILE_READ_ATTRIBUTES);
    return plain_directory(batch) && marker_matches(batch_directory(id), id);
#else
    (void)id; return false;
#endif
}
bool ClipboardCopyStore::open_batch(std::string_view id, std::string* error) {
    if (!valid_id(id)) return fail(error, "clipboard_copy_id_invalid");
#ifdef _WIN32
    std::vector<File> parents;
    if (!pin_ancestors(directory_, parents, error)) return false;
    const auto batch = batch_directory(id);
    auto handle = open(batch, FILE_READ_ATTRIBUTES);
    if (!plain_directory(handle) || !marker_matches(batch, id)) return fail(error, "clipboard_copy_batch_not_owned");
    const auto files = batch / L"files";
    auto files_handle = open(files, FILE_READ_ATTRIBUTES);
    if (!plain_directory(files_handle)) return fail(error, "clipboard_copy_files_unavailable");
    // Open only this verified directory, never an executable/shortcut or a
    // command string supplied by the peer. The ordinary user token is retained.
    if (reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"explore", files.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        return fail(error, "clipboard_copy_open_failed");
    if (error) error->clear(); return true;
#else
    return fail(error, "clipboard_platform_unsupported");
#endif
}
bool ClipboardCopyStore::cleanup_batch(std::string_view id, const std::atomic_bool& cancelled,
    const std::function<void(const ClipboardCopyCleanupProgress&)>& progress, std::string* error) {
    if (!valid_id(id)) return fail(error, "clipboard_copy_id_invalid");
#ifdef _WIN32
    if (cancelled.load()) return fail(error, "clipboard_copy_cleanup_cancelled");
    std::vector<File> ancestors;
    if (!pin_ancestors(directory_, ancestors, error)) return false;
    const auto path = batch_directory(id);
    auto root = open(path, FILE_READ_ATTRIBUTES | DELETE);
    if (!root && (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) {
        if (error) error->clear(); return true;
    }
    if (!plain_directory(root) || !marker_matches(path, id)) return fail(error, "clipboard_copy_batch_not_owned");
    std::vector<DirectoryFrame> stack;
    stack.push_back({path, std::move(root)});
    ClipboardCopyCleanupProgress totals;
    while (!stack.empty()) {
        if (cancelled.load()) return fail(error, "clipboard_copy_cleanup_cancelled");
        auto& frame = stack.back();
        bool next = false;
        if (!frame.started) {
            frame.started = true;
            const auto search = FindFirstFileW((frame.path / L"*").c_str(), &frame.entry);
            if (search != INVALID_HANDLE_VALUE) { frame.search.reset(search); next = true; }
            else if (GetLastError() != ERROR_FILE_NOT_FOUND) return fail(error, "clipboard_copy_enumeration_failed");
        } else if (frame.search) {
            next = FindNextFileW(frame.search.get(), &frame.entry) != FALSE;
            if (!next && GetLastError() != ERROR_NO_MORE_FILES) return fail(error, "clipboard_copy_enumeration_failed");
        }
        if (!next) {
            frame.search.reset();
            if (stack.size() == 1) {
                // Keep the ownership receipt until all children are gone, so
                // a cancelled/occupied-file cleanup can be safely retried.
                auto marker = open(path / L".redclaw-clipboard-batch", DELETE);
                if (!marker || !remove_handle(marker, error)) return false;
            }
            if (!remove_handle(frame.handle, error)) {
                // A concurrent creator can make the root nonempty after its
                // final enumeration. Restore ownership for a later retry.
                if (stack.size() == 1 && !write_marker(path, id, nullptr)) return fail(error, "clipboard_copy_receipt_restore_failed");
                return false;
            }
            ++totals.directories; stack.pop_back();
        } else {
            const std::wstring_view name(frame.entry.cFileName);
            if (name == L"." || name == L".." || (stack.size() == 1 && name == L".redclaw-clipboard-batch")) continue;
            const auto child = frame.path / name;
            auto handle = open(child, FILE_READ_ATTRIBUTES | DELETE);
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            if (!handle || !GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)))
                return fail(error, "clipboard_copy_file_in_use_or_denied");
            if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                stack.push_back({child, std::move(handle)}); continue;
            }
            // A symlink/junction itself may be removed; its target is never opened.
            if (!remove_handle(handle, error)) return false;
            ++totals.files;
        }
        if (progress) progress(totals);
    }
    if (error) error->clear(); return true;
#else
    (void)cancelled; (void)progress; return fail(error, "clipboard_platform_unsupported");
#endif
}
}
