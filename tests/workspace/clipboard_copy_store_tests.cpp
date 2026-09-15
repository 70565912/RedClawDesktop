#include "redclaw/workspace/clipboard_copy_store.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <winioctl.h>
#endif

namespace {
using namespace redclaw::workspace;
class ClipboardCopies : public testing::Test {
protected:
    std::filesystem::path root, junction;
    std::unique_ptr<ClipboardCopyStore> store;
    std::atomic_bool cancelled{false};
    std::string error, id;
    void SetUp() override {
#ifndef _WIN32
        GTEST_SKIP() << "Windows owned copy cleanup.";
#else
        root = std::filesystem::canonical(std::filesystem::temp_directory_path())
            / ("redclaw-copy-store-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directory(root));
        store = std::make_unique<ClipboardCopyStore>(root); id = ClipboardCopyStore::new_id();
        ASSERT_TRUE(store->create_batch(id, &error)) << error;
#endif
    }
    void TearDown() override {
#ifdef _WIN32
        if (!junction.empty()) RemoveDirectoryW(junction.c_str()); // unlink only; never traverse the fixture's target
#endif
        if (root.empty()) return;
        std::error_code ec;
        const auto resolved = std::filesystem::canonical(root, ec);
        const auto parent = std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
        if (!ec && resolved == root && resolved.parent_path() == parent && root.filename().string().starts_with("redclaw-copy-store-test-"))
            std::filesystem::remove_all(resolved, ec);
    }
};
TEST_F(ClipboardCopies, IdBoundaryOwnershipAndCancellationRetainRetryableCopies) {
    ASSERT_TRUE(store); const auto batch = store->batch_directory(id);
    EXPECT_FALSE(store->create_batch(id, &error));
    EXPECT_FALSE(store->cleanup_batch("../escape", cancelled, {}, &error));
    EXPECT_FALSE(store->cleanup_batch("C:/Windows", cancelled, {}, &error));
    std::filesystem::create_directories(batch / "files/empty");
    { std::ofstream file(batch / "files/first.txt"); file << "first"; }
    { std::ofstream file(batch / "files/second.txt"); file << "second"; }
    EXPECT_FALSE(store->cleanup_batch(id, cancelled, [&](const auto& progress) {
        if (progress.files == 1) cancelled.store(true);
    }, &error));
    EXPECT_EQ(error, "clipboard_copy_cleanup_cancelled");
    EXPECT_TRUE(std::filesystem::exists(batch / ".redclaw-clipboard-batch"));
    EXPECT_TRUE(std::filesystem::is_directory(batch));
    cancelled.store(false);
    ASSERT_TRUE(store->cleanup_batch(id, cancelled, {}, &error)) << error;
    EXPECT_FALSE(std::filesystem::exists(batch));
    EXPECT_TRUE(store->cleanup_batch(id, cancelled, {}, &error)) << error;
    const auto foreign = ClipboardCopyStore::new_id();
    ASSERT_TRUE(std::filesystem::create_directory(store->batch_directory(foreign)));
    { std::ofstream file(store->batch_directory(foreign) / "retain.txt"); file << "not a received batch"; }
    EXPECT_FALSE(store->cleanup_batch(foreign, cancelled, {}, &error));
    EXPECT_TRUE(std::filesystem::exists(store->batch_directory(foreign) / "retain.txt"));
}
TEST_F(ClipboardCopies, OccupiedFilePreventsCleanupWithoutLosingOwnershipReceipt) {
#ifdef _WIN32
    const auto batch = store->batch_directory(id);
    const auto path = batch / L"occupied.txt";
    { std::ofstream file(path); file << "hold"; }
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    EXPECT_FALSE(store->cleanup_batch(id, cancelled, {}, &error));
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_TRUE(std::filesystem::exists(batch / ".redclaw-clipboard-batch"));
    CloseHandle(handle);
    EXPECT_TRUE(store->cleanup_batch(id, cancelled, {}, &error)) << error;
#endif
}
TEST_F(ClipboardCopies, JunctionTargetOutsideBatchIsNeverTraversed) {
#ifdef _WIN32
    const auto outside = root / "outside";
    ASSERT_TRUE(std::filesystem::create_directory(outside));
    { std::ofstream file(outside / "retain.txt"); file << "must survive"; }
    junction = store->batch_directory(id) / "junction";
    ASSERT_TRUE(std::filesystem::create_directory(junction));
    const auto handle = CreateFileW(junction.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    struct Header { DWORD tag; WORD length, reserved, substitute_offset, substitute_length, print_offset, print_length; };
    static_assert(sizeof(Header) == 16);
    const auto substitute = L"\\??\\" + outside.wstring(); const auto print = outside.wstring();
    const auto substitute_bytes = substitute.size() * sizeof(wchar_t), print_bytes = print.size() * sizeof(wchar_t);
    std::vector<unsigned char> buffer(sizeof(Header) + substitute_bytes + print_bytes + 2 * sizeof(wchar_t));
    Header header{IO_REPARSE_TAG_MOUNT_POINT, static_cast<WORD>(buffer.size() - 8), 0, 0,
        static_cast<WORD>(substitute_bytes), static_cast<WORD>(substitute_bytes + sizeof(wchar_t)), static_cast<WORD>(print_bytes)};
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), substitute.c_str(), substitute_bytes);
    std::memcpy(buffer.data() + sizeof(header) + header.print_offset, print.c_str(), print_bytes);
    DWORD written = 0; const bool linked = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
        static_cast<DWORD>(buffer.size()), nullptr, 0, &written, nullptr) != FALSE;
    const auto native_error = GetLastError(); CloseHandle(handle);
    if (!linked) GTEST_SKIP() << "Fixture junction unavailable: " << native_error;
    EXPECT_TRUE(store->cleanup_batch(id, cancelled, {}, &error)) << error;
    EXPECT_FALSE(std::filesystem::exists(store->batch_directory(id)));
    EXPECT_TRUE(std::filesystem::exists(outside / "retain.txt"));
#endif
}
}
