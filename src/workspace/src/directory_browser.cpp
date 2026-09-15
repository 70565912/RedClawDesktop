#include "redclaw/workspace/directory_browser.h"
#include "redclaw/workspace/clipboard_copy_store.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace redclaw::workspace {
DirectoryBrowser::DirectoryBrowser(std::filesystem::path directory, bool clipboard_copies)
    : thread_([this, directory = std::move(directory), clipboard_copies] { run(directory, clipboard_copies); }) {}
DirectoryBrowser::~DirectoryBrowser() { cancel(); if (thread_.joinable()) thread_.join(); }
void DirectoryBrowser::cancel() noexcept {
    cancelled_.store(true); wake_.notify_one();
#ifdef _WIN32
    if (thread_.joinable()) (void)CancelSynchronousIo(thread_.native_handle());
#endif
}
std::optional<protocol::WorkspaceControlV1> DirectoryBrowser::take() {
    std::lock_guard lock(mutex_);
    if (cancelled_.load() || entries_.empty()) return {};
    auto result = std::move(entries_.front()); entries_.pop_front(); wake_.notify_one(); return result;
}
bool DirectoryBrowser::emit(protocol::WorkspaceControlV1 message) {
    std::unique_lock lock(mutex_);
    wake_.wait(lock, [this] { return cancelled_.load() || entries_.size() < 16; });
    if (cancelled_.load()) return false;
    entries_.push_back(std::move(message)); return true;
}
void DirectoryBrowser::run(std::filesystem::path directory, bool clipboard_copies) {
    std::string error;
    try {
#ifdef _WIN32
        const auto add = [this](const std::filesystem::path& path, bool is_directory, std::uint64_t size) {
            protocol::WorkspaceControlV1 message; message.action = protocol::WorkspaceActionV1::kBrowseEntry;
            const auto bytes = path.generic_u8string(); message.path.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            message.directory = is_directory; message.entries = 1; message.files = is_directory ? 0 : 1; message.bytes = size;
            return emit(std::move(message));
        };
        if (directory.empty()) {
            const DWORD drives = GetLogicalDrives();
            if (!drives) error = "browse_drives_unavailable";
            for (unsigned index = 0; index < 26 && !cancelled_.load(); ++index) {
                if (!(drives & (1U << index))) continue;
                std::wstring root{static_cast<wchar_t>(L'A' + index), L':', L'\\'};
                if (!add(root, true, 0)) break;
            }
        } else if (!directory.is_absolute()) error = "browse_path_not_absolute";
        else {
            std::error_code ec;
            std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::none, ec), end;
            if (ec && !(clipboard_copies && ec == std::errc::no_such_file_or_directory)) error = "browse_directory_unavailable";
            ClipboardCopyStore copies(directory.parent_path());
            while (error.empty() && !cancelled_.load() && it != end) {
                const auto attributes = GetFileAttributesW(it->path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES) { error = "browse_entry_unavailable"; break; }
                const bool folder = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                const auto size = folder ? 0 : it->file_size(ec);
                if (ec) { error = "browse_entry_unavailable"; break; }
                if (clipboard_copies) {
                    const auto name = it->path().filename().generic_u8string();
                    const std::string id(reinterpret_cast<const char*>(name.data()), name.size());
                    const auto files_attributes = GetFileAttributesW((it->path() / L"files").c_str());
                    if (folder && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) && copies.contains_batch(id)
                        && files_attributes != INVALID_FILE_ATTRIBUTES && (files_attributes & FILE_ATTRIBUTE_DIRECTORY)
                        && !(files_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                        protocol::WorkspaceControlV1 entry; entry.action = protocol::WorkspaceActionV1::kBrowseEntry;
                        entry.path = id; entry.directory = true; entry.entries = 1;
                        WIN32_FILE_ATTRIBUTE_DATA data{};
                        if (GetFileAttributesExW(it->path().c_str(), GetFileExInfoStandard, &data)) {
                            const auto ticks = (static_cast<std::uint64_t>(data.ftCreationTime.dwHighDateTime) << 32) | data.ftCreationTime.dwLowDateTime;
                            if (ticks >= 116444736000000000ULL) entry.created_at_ms = (ticks - 116444736000000000ULL) / 10000;
                        }
                        if (!emit(std::move(entry))) break;
                    }
                } else if (!add(it->path(), folder, size)) break;
                it.increment(ec); if (ec) error = "browse_directory_changed";
            }
        }
#else
        (void)directory; (void)clipboard_copies; error = "browse_unsupported";
#endif
    } catch (const std::exception&) { error = "browse_failed"; }
    protocol::WorkspaceControlV1 end; end.action = protocol::WorkspaceActionV1::kBrowseEnd; end.error_code = std::move(error);
    (void)emit(std::move(end)); finished_.store(true, std::memory_order_release);
}
}
