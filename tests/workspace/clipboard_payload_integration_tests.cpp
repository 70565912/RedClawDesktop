#include "redclaw/workspace/clipboard_payload.h"
#include "redclaw/workspace/clipboard_paste_guard.h"
#include "redclaw/workspace/transfer_file_source.h"
#include "redclaw_wire.pb.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Shellapi.h>
#include <ShlObj.h>
#endif

namespace {
using namespace redclaw::workspace;
TEST(ClipboardNativeFocus, LeavingAndReturningToOriginalWindowInvalidatesPendingPaste) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows foreground focus tracking.";
#else
    // Native EDIT controls can create thread-owned IME windows. Run this
    // fixture on a fresh thread so later window-station tests inherit none.
    std::thread fixture([] {
    struct OwnedWindow {
        HWND value;
        ~OwnedWindow() { if (value) DestroyWindow(value); }
    };
    OwnedWindow window{CreateWindowExW(0, L"STATIC", L"RedClaw focus validation", WS_OVERLAPPEDWINDOW,
        50, 50, 420, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
    ASSERT_NE(window.value, nullptr);
    const auto first = CreateWindowExW(0, L"EDIT", L"first", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        12, 12, 200, 28, window.value, nullptr, GetModuleHandleW(nullptr), nullptr);
    const auto second = CreateWindowExW(0, L"EDIT", L"second", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        12, 52, 200, 28, window.value, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_TRUE(first && second);
    ShowWindow(window.value, SW_SHOWNORMAL); SetForegroundWindow(window.value); SetFocus(first);
    if (GetForegroundWindow() != window.value) GTEST_SKIP() << "Own-window foreground guard denied the fixture.";
    ClipboardFocusTracker tracker;
    std::string error;
    auto focus = tracker.snapshot(&error);
    ASSERT_TRUE(focus) << error; EXPECT_EQ(focus->focus, reinterpret_cast<std::uintptr_t>(first));
    EXPECT_TRUE(tracker.pump_events());
    SetFocus(second); SetFocus(first);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool unchanged = true;
    while (unchanged && std::chrono::steady_clock::now() < deadline) {
        unchanged = tracker.pump_events();
        if (unchanged) MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    EXPECT_FALSE(unchanged); // same HWND again is still a changed focus generation
    const auto current = tracker.snapshot(&error);
    ASSERT_TRUE(current) << error;
    EXPECT_EQ(current->focus, focus->focus); EXPECT_GT(current->generation, focus->generation);
    });
    fixture.join();
#endif
}
class ClipboardPayloadFixture : public testing::Test {
protected:
    std::filesystem::path root;
    std::atomic_bool cancelled{false};
    std::string error;
#ifdef _WIN32
    HWINSTA original_station = nullptr, station = nullptr;
    HDESK original_desktop = nullptr, desktop = nullptr;
    HWND owner = nullptr;
    bool isolated = false, station_changed = false, desktop_changed = false;
    bool put(UINT format, const void* bytes, std::size_t size) {
        const auto memory = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!memory) return false;
        auto* target = GlobalLock(memory);
        if (!target) { GlobalFree(memory); return false; }
        std::memcpy(target, bytes, size); GlobalUnlock(memory);
        if (!SetClipboardData(format, memory)) { GlobalFree(memory); return false; }
        return true;
    }
    bool set_text(const std::wstring& text) {
        if (!isolated || !OpenClipboard(owner)) return false;
        const bool ok = EmptyClipboard() && put(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        CloseClipboard(); return ok;
    }
    std::wstring text() {
        if (!isolated || !OpenClipboard(owner)) return {};
        const auto value = GetClipboardData(CF_UNICODETEXT);
        const auto* memory = value ? static_cast<const wchar_t*>(GlobalLock(value)) : nullptr;
        const std::wstring result = memory ? memory : L"";
        if (memory) GlobalUnlock(value); CloseClipboard(); return result;
    }
    bool add_formats() {
        if (!OpenClipboard(owner)) return false;
        const char rtf[] = "{\\rtf1\\ansi RedClaw fixture}";
        const char html[] = "Version:1.0\r\nStartHTML:-1\r\nEndHTML:-1\r\nStartFragment:0000000105\r\nEndFragment:0000000119\r\n<b>fixture</b>";
        struct Bitmap { BITMAPINFOHEADER header; unsigned char pixel[4]; } bitmap{};
        bitmap.header.biSize = sizeof(BITMAPINFOHEADER); bitmap.header.biWidth = bitmap.header.biHeight = 1;
        bitmap.header.biPlanes = 1; bitmap.header.biBitCount = 32; bitmap.header.biSizeImage = 4;
        bitmap.pixel[0] = 0x71; bitmap.pixel[1] = 0x29; bitmap.pixel[2] = 0xde; bitmap.pixel[3] = 0xff;
        const bool ok = put(RegisterClipboardFormatW(L"Rich Text Format"), rtf, sizeof(rtf))
            && put(RegisterClipboardFormatW(L"HTML Format"), html, sizeof(html)) && put(CF_DIB, &bitmap, sizeof(bitmap));
        CloseClipboard(); return ok;
    }
    void stage(const ClipboardSnapshot& snapshot) {
        std::filesystem::create_directory(root / "received");
        std::filesystem::copy(snapshot.metadata_directory(), root / "received/metadata", std::filesystem::copy_options::recursive);
        for (const auto& item : snapshot.files()) {
            const auto target = root / "received" / std::filesystem::path(std::u8string(
                reinterpret_cast<const char8_t*>(item.relative_root.data()), item.relative_root.size()));
            std::filesystem::create_directories(target.parent_path());
            std::filesystem::copy(item.source, target, std::filesystem::copy_options::recursive);
        }
    }
#endif
    void SetUp() override {
#ifndef _WIN32
        GTEST_SKIP() << "Windows clipboard adapter.";
#else
        original_station = GetProcessWindowStation(); original_desktop = GetThreadDesktop(GetCurrentThreadId());
        // Each window station has its own clipboard. CREATE_ONLY prevents the
        // fixture from joining an existing station or modifying its contents.
        const auto station_name = L"RedClawClipboardFixture-" + std::to_wstring(GetCurrentProcessId()) + L"-"
            + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
        station = CreateWindowStationW(station_name.c_str(), CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, nullptr);
        if (!station) GTEST_SKIP() << "An exclusive test window station is unavailable: " << GetLastError();
        ASSERT_TRUE(SetProcessWindowStation(station));
        station_changed = true;
        desktop = CreateDesktopW(L"RedClawClipboardFixture", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        ASSERT_NE(desktop, nullptr); ASSERT_TRUE(SetThreadDesktop(desktop)); desktop_changed = true;
        owner = CreateWindowExW(0, L"STATIC", L"Clipboard fixture", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        ASSERT_NE(owner, nullptr); isolated = true;
        root = std::filesystem::canonical(std::filesystem::temp_directory_path())
            / ("redclaw-clipboard-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directory(root));
#endif
    }
    void TearDown() override {
#ifdef _WIN32
        if (owner) DestroyWindow(owner);
        if (desktop_changed) EXPECT_TRUE(SetThreadDesktop(original_desktop));
        if (station_changed) EXPECT_TRUE(SetProcessWindowStation(original_station));
        if (desktop) EXPECT_TRUE(CloseDesktop(desktop));
        if (station) EXPECT_TRUE(CloseWindowStation(station));
#endif
        if (!root.empty()) {
            std::error_code ec;
            const auto checked = std::filesystem::canonical(root, ec);
            const auto parent = std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
            if (!ec && checked == root && checked.parent_path() == parent && checked.filename().string().starts_with("redclaw-clipboard-fixture-"))
                std::filesystem::remove_all(checked, ec);
        }
    }
};
class ClipboardPreparedFilesFixture : public testing::Test {
protected:
    std::filesystem::path root;
    std::atomic_bool cancelled{false};
    std::string error;
    redclaw::protocol::wire::ClipboardPayloadManifestV1 manifest;
    void SetUp() override {
#ifndef _WIN32
        GTEST_SKIP() << "Windows clipboard object allocation.";
#else
        root = std::filesystem::canonical(std::filesystem::temp_directory_path())
            / ("redclaw-prepared-clipboard-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directories(root / "metadata"));
        manifest.set_schema_version(1);
        const std::u16string text = u"verified \u4e2d\u6587";
        { std::ofstream file(root / "metadata/format-1.bin", std::ios::binary);
          file.write(reinterpret_cast<const char*>(text.c_str()), static_cast<std::streamsize>((text.size() + 1) * sizeof(char16_t))); }
        TransferFileSource source;
        ASSERT_TRUE(source.open(root / "metadata/format-1.bin", &error)) << error;
        auto* format = manifest.add_formats(); format->set_kind(1); format->set_size(source.size());
        for (;;) {
            const auto read = source.read(); ASSERT_NE(read.state, TransferReadState::kFailed) << read.error;
            if (read.state == TransferReadState::kComplete) { format->set_sha256(read.sha256); break; }
        }
        write_manifest();
#endif
    }
    void write_manifest() {
        std::ofstream file(root / "metadata/manifest.pb", std::ios::binary);
        const auto bytes = manifest.SerializeAsString(); file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    void TearDown() override {
        if (root.empty()) return;
        std::error_code ec;
        const auto checked = std::filesystem::canonical(root, ec);
        const auto parent = std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
        if (!ec && checked == root && checked.parent_path() == parent && checked.filename().string().starts_with("redclaw-prepared-clipboard-fixture-"))
            std::filesystem::remove_all(checked, ec);
    }
};
TEST_F(ClipboardPreparedFilesFixture, VerifiedDataAllocatesObjectsAndTamperedDataFailsBeforePublication) {
    PreparedClipboardPayload valid;
    ASSERT_TRUE(valid.load(root, cancelled, &error)) << error;
    EXPECT_FALSE(valid.publish({}, &error)); EXPECT_EQ(error, "clipboard_payload_not_ready");
    { std::fstream file(root / "metadata/format-1.bin", std::ios::binary | std::ios::in | std::ios::out); file.put('x'); }
    PreparedClipboardPayload damaged;
    EXPECT_FALSE(damaged.load(root, cancelled, &error)); EXPECT_EQ(error, "clipboard_payload_checksum_mismatch");
    EXPECT_FALSE(damaged.publish({}, &error));
}
TEST_F(ClipboardPreparedFilesFixture, UnknownVersionDuplicateFormatsAndCancellationAreRejected) {
    manifest.set_schema_version(2); write_manifest();
    PreparedClipboardPayload version; EXPECT_FALSE(version.load(root, cancelled, &error));
    EXPECT_EQ(error, "clipboard_manifest_version_or_format_invalid");
    manifest.set_schema_version(1); *manifest.add_formats() = manifest.formats(0); write_manifest();
    PreparedClipboardPayload duplicate; EXPECT_FALSE(duplicate.load(root, cancelled, &error)); EXPECT_EQ(error, "clipboard_manifest_invalid");
    manifest.mutable_formats()->RemoveLast(); write_manifest();
    cancelled.store(true); PreparedClipboardPayload cancelled_payload;
    EXPECT_FALSE(cancelled_payload.load(root, cancelled, &error)); EXPECT_EQ(error, "clipboard_cancelled");
}
TEST_F(ClipboardPayloadFixture, NativeTextRichTextAndImageRoundTripWithoutTouchingInteractiveClipboard) {
#ifdef _WIN32
    const std::wstring expected = L"RedClaw \u4e2d\u6587 \U0001f34e\r\nclipboard fixture";
    ASSERT_TRUE(set_text(expected)); ASSERT_TRUE(add_formats());
    ClipboardSnapshot snapshot;
    ASSERT_TRUE(snapshot.capture(root, GetClipboardSequenceNumber(), cancelled, &error)) << error;
    EXPECT_EQ(text(), expected); EXPECT_TRUE(snapshot.files().empty()); stage(snapshot);
    ASSERT_TRUE(set_text(L"sentinel"));
    PreparedClipboardPayload payload;
    ASSERT_TRUE(payload.load(root / "received", cancelled, &error)) << error;
    EXPECT_EQ(text(), L"sentinel");
    int guard_calls = 0;
    ASSERT_TRUE(payload.publish([&] { ++guard_calls; return true; }, &error)) << error;
    EXPECT_EQ(guard_calls, 1); EXPECT_EQ(text(), expected);
    EXPECT_TRUE(IsClipboardFormatAvailable(RegisterClipboardFormatW(L"Rich Text Format")));
    EXPECT_TRUE(IsClipboardFormatAvailable(RegisterClipboardFormatW(L"HTML Format")));
    EXPECT_TRUE(IsClipboardFormatAvailable(CF_DIB));
    EXPECT_FALSE(payload.publish([&] { ++guard_calls; return true; }, &error)); EXPECT_EQ(guard_calls, 1);
#endif
}
TEST_F(ClipboardPayloadFixture, FocusRejectionChecksumFailureAndCancelledSnapshotLeaveClipboardUntouched) {
#ifdef _WIN32
    ASSERT_TRUE(set_text(L"snapshot"));
    ClipboardSnapshot snapshot; ASSERT_TRUE(snapshot.capture(root, GetClipboardSequenceNumber(), cancelled, &error)) << error;
    stage(snapshot); ASSERT_TRUE(set_text(L"sentinel"));
    PreparedClipboardPayload blocked;
    ASSERT_TRUE(blocked.load(root / "received", cancelled, &error)) << error;
    EXPECT_FALSE(blocked.publish([] { return false; }, &error)); EXPECT_EQ(error, "clipboard_paste_invalidated");
    EXPECT_EQ(text(), L"sentinel");
    { std::fstream file(root / "received/metadata/format-1.bin", std::ios::binary | std::ios::in | std::ios::out); file.put('x'); }
    PreparedClipboardPayload damaged; EXPECT_FALSE(damaged.load(root / "received", cancelled, &error));
    EXPECT_EQ(error, "clipboard_payload_checksum_mismatch"); EXPECT_EQ(text(), L"sentinel");
    cancelled.store(true); ClipboardSnapshot cancelled_snapshot;
    EXPECT_FALSE(cancelled_snapshot.capture(root, GetClipboardSequenceNumber(), cancelled, &error)); EXPECT_EQ(text(), L"sentinel");
#endif
}
TEST_F(ClipboardPayloadFixture, FileListsUseReceivedCopiesAndPreserveSourceFilesAndEmptyDirectories) {
#ifdef _WIN32
    std::filesystem::create_directories(root / "source/folder/empty");
    { std::ofstream file(root / L"source/\u6587\u5b57.txt"); file << "fixture"; }
    const std::vector<std::filesystem::path> selected{root / L"source/\u6587\u5b57.txt", root / "source/folder"};
    std::size_t bytes = sizeof(DROPFILES) + sizeof(wchar_t);
    for (const auto& item : selected) bytes += (item.wstring().size() + 1) * sizeof(wchar_t);
    std::vector<unsigned char> drop(bytes, 0);
    auto* header = reinterpret_cast<DROPFILES*>(drop.data()); header->pFiles = sizeof(DROPFILES); header->fWide = TRUE;
    auto* path = reinterpret_cast<wchar_t*>(drop.data() + sizeof(DROPFILES));
    for (const auto& item : selected) { const auto value = item.wstring(); std::memcpy(path, value.c_str(), (value.size() + 1) * sizeof(wchar_t)); path += value.size() + 1; }
    ASSERT_TRUE(OpenClipboard(owner)); const bool seeded = EmptyClipboard() && put(CF_HDROP, drop.data(), drop.size()); CloseClipboard(); ASSERT_TRUE(seeded);
    ClipboardSnapshot snapshot; ASSERT_TRUE(snapshot.capture(root, GetClipboardSequenceNumber(), cancelled, &error)) << error;
    ASSERT_EQ(snapshot.files().size(), 2U); stage(snapshot);
    PreparedClipboardPayload payload; ASSERT_TRUE(payload.load(root / "received", cancelled, &error)) << error;
    ASSERT_TRUE(payload.publish([] { return true; }, &error)) << error;
    ASSERT_TRUE(OpenClipboard(owner));
    const auto received = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    const auto count = received ? DragQueryFileW(received, 0xffffffffU, nullptr, 0) : 0;
    std::vector<std::filesystem::path> paths;
    for (UINT i = 0; i < count; ++i) {
        const auto length = DragQueryFileW(received, i, nullptr, 0); std::wstring value(length + 1, L'\0');
        DragQueryFileW(received, i, value.data(), length + 1); value.resize(length); paths.emplace_back(value);
    }
    CloseClipboard(); ASSERT_EQ(paths.size(), 2U);
    for (std::size_t i = 0; i < paths.size(); ++i) {
        EXPECT_EQ(paths[i].filename(), selected[i].filename()); EXPECT_TRUE(std::filesystem::exists(paths[i]));
        EXPECT_TRUE(std::filesystem::exists(selected[i])); EXPECT_NE(paths[i], selected[i]);
    }
    EXPECT_TRUE(std::filesystem::is_directory(paths[1] / "empty"));
#endif
}
}
