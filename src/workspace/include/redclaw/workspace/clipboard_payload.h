#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace redclaw::workspace {
enum class ClipboardPayloadFormat : std::uint32_t { kUnicodeText = 1, kHtml, kRtf, kPng, kDibV5, kDib };
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
        const std::atomic_bool& cancelled, std::string* error);
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
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
using ClipboardCapture = std::function<bool(ClipboardSnapshot&, const std::filesystem::path&, std::uint32_t,
    const std::atomic_bool&, std::string*)>;
}
