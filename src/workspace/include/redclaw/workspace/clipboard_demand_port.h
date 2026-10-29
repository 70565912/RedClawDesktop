#pragma once
#include "redclaw/workspace/clipboard_description.h"
#include <optional>
#include <functional>

namespace redclaw::workspace {
inline constexpr std::uint32_t kClipboardFilesFormat = 64;
struct ClipboardDemandOffer {
    std::string token;
    std::uint32_t formats = 0, sequence = 0;
    bool move = false;
};
enum class ClipboardDemandEventKind { kOffer, kShortcutReady, kDescribe, kDemand, kRead, kNativeResult, kInstalled, kMoveReceipt, kUnavailable };
struct ClipboardDemandEvent {
    ClipboardDemandEventKind kind = ClipboardDemandEventKind::kOffer;
    std::string operation;
    ClipboardDemandOffer offer;
    bool complete = false;
    std::uint32_t effect = 0;
    std::string error;
};
// A single native STA owns OLE objects. The runtime only enqueues commands and
// takes bounded events. Source paths and native identities never cross the wire.
class ClipboardDemandPort {
public:
    virtual ~ClipboardDemandPort() = default;
    virtual void connected(bool enabled, std::string epoch) = 0;
    virtual std::optional<ClipboardDemandEvent> take_event() = 0;
    virtual void remote_offer(ClipboardDemandOffer offer, std::string invoke = {}) = 0;
    virtual void refresh_for_shortcut(std::string operation, std::uint32_t sequence) = 0;
    virtual bool source(std::string_view token, std::uint32_t& sequence, std::vector<ClipboardFileSelection>& files) = 0;
    virtual void description(std::string operation, std::shared_ptr<ClipboardDescription> description, std::string error) = 0;
    virtual void approve(std::string operation) = 0;
    virtual void payload(std::string operation, std::shared_ptr<PreparedClipboardPayload> payload,
        std::filesystem::path directory, std::string error) = 0;
    virtual void cancel(std::string operation) = 0;
    virtual void finished(std::string, bool) {}
    virtual void commit_source(std::string operation, std::string token,
        std::shared_ptr<ClipboardDescription> description) = 0;
};
std::shared_ptr<ClipboardDemandPort> make_native_clipboard_demand_port();
bool clipboard_is_remote_offer();
bool clipboard_has_files();
// Native fixture uses a detached IDataObject, never the interactive clipboard.
// Returns IUnknown with one owned reference on Windows, null elsewhere.
void* make_clipboard_data_object_for_test(ClipboardDemandOffer offer,
    std::function<void(ClipboardDemandEvent)> event,
    std::function<void(std::shared_ptr<PreparedClipboardPayload>, std::shared_ptr<ClipboardDescription>, std::filesystem::path)>* supply);
}
