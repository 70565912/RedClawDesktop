#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace redclaw::workspace {
[[nodiscard]] std::uint32_t clipboard_sequence_number();
enum class ClipboardPayloadFormat : std::uint32_t { kUnicodeText = 1, kHtml, kRtf, kPng, kDibV5, kDib };
constexpr std::uint32_t clipboard_format_bit(ClipboardPayloadFormat kind) {
    const auto value = static_cast<std::uint32_t>(kind);
    return value >= 1 && value <= 6 ? 1U << (value - 1) : 0;
}
inline constexpr std::uint32_t kClipboardImageFormats = clipboard_format_bit(ClipboardPayloadFormat::kPng)
    | clipboard_format_bit(ClipboardPayloadFormat::kDibV5) | clipboard_format_bit(ClipboardPayloadFormat::kDib);
inline constexpr std::uint32_t kClipboardFilesFormat = 64;
inline constexpr std::uint32_t kClipboardContentFormats = kClipboardFilesFormat - 1;
// One wire representation can have more than one Windows clipboard name.
// The first ID is the canonical native format, the second is an optional alias.
[[nodiscard]] std::array<std::uint32_t, 2> clipboard_native_formats(ClipboardPayloadFormat kind);
struct ClipboardFileSelection {
    std::filesystem::path source;
    std::string relative_root; // files/<index>/<original name>; never the source's absolute path
};
// Run on the existing transfer disk worker. Clipboard format bytes are copied
// in bounded chunks into a snapshot, not placed on a GUI/control queue. Only
// selected file roots (not their recursive trees or contents) stay in memory.
class ClipboardSnapshot final {
public:
    ClipboardSnapshot();
    ~ClipboardSnapshot();
    bool capture(const std::filesystem::path& spool_directory, std::uint32_t expected_sequence,
        const std::atomic_bool& cancelled, std::string* error,
        std::optional<std::uint32_t> selected_formats = std::nullopt);
    bool create(const std::filesystem::path& spool_directory, std::string_view descriptor,
        const std::atomic_bool& cancelled, std::string* error);
    [[nodiscard]] const std::filesystem::path& metadata_directory() const;
    [[nodiscard]] std::span<const ClipboardFileSelection> files() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Allocates/prepares Windows clipboard objects before touching the clipboard.
// Windows requires complete HGLOBAL objects at publication; allocation/hash
// failure leaves the existing clipboard untouched. The caller owns the one-shot
// focus/eligibility guard and subsequent input injection.
class PreparedClipboardPayload final {
public:
    PreparedClipboardPayload();
    ~PreparedClipboardPayload();
    bool load(const std::filesystem::path& received_directory, const std::atomic_bool& cancelled, std::string* error);
    bool publish(const std::function<bool()>& final_guard, std::string* error);
    // Returns an independent HGLOBAL for the native OLE consumer; never publishes
    // or takes clipboard ownership. Zero means unavailable. Caller frees it.
    [[nodiscard]] std::uintptr_t clone_native_format(ClipboardPayloadFormat format) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
using ClipboardCapture = std::function<bool(ClipboardSnapshot&, const std::filesystem::path&, std::uint32_t,
    const std::atomic_bool&, std::string*)>;
}
