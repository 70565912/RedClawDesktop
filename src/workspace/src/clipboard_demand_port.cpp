#include "redclaw/workspace/clipboard_demand_port.h"
#include "redclaw/workspace/clipboard_copy_store.h"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shldisp.h>
#include <wrl/client.h>
#endif

namespace redclaw::workspace {
#ifdef _WIN32
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t kRemoteFormat[] = L"RedClaw.ClipboardOffer.v5";
UINT format(ClipboardPayloadFormat type) {
    switch (type) {
    case ClipboardPayloadFormat::kUnicodeText: return CF_UNICODETEXT;
    case ClipboardPayloadFormat::kHtml: return RegisterClipboardFormatW(L"HTML Format");
    case ClipboardPayloadFormat::kRtf: return RegisterClipboardFormatW(L"Rich Text Format");
    case ClipboardPayloadFormat::kPng: return RegisterClipboardFormatW(L"PNG");
    case ClipboardPayloadFormat::kDibV5: return CF_DIBV5;
    case ClipboardPayloadFormat::kDib: return CF_DIB;
    }
    return 0;
}
bool ordinary_desktop() {
    const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!input) return false;
    wchar_t name[64]{}; DWORD needed = 0;
    const bool ok = GetUserObjectInformationW(input, UOI_NAME, name, sizeof(name), &needed)
        && CompareStringOrdinal(name, -1, L"Default", -1, TRUE) == CSTR_EQUAL;
    CloseDesktop(input); return ok;
}
std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) return {};
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}
HRESULT global_medium(STGMEDIUM* result, const void* bytes, std::size_t size) {
    const auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    if (!memory) return E_OUTOFMEMORY;
    auto* destination = GlobalLock(memory);
    if (!destination) { GlobalFree(memory); return E_OUTOFMEMORY; }
    if (bytes) std::memcpy(destination, bytes, size);
    GlobalUnlock(memory); result->tymed = TYMED_HGLOBAL; result->hGlobal = memory; result->pUnkForRelease = nullptr;
    return S_OK;
}
class FormatEnumerator final : public IEnumFORMATETC {
public:
    explicit FormatEnumerator(std::vector<FORMATETC> values, std::size_t position = 0) : values_(std::move(values)), position_(position) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override {
        if (!value) return E_POINTER; *value = nullptr;
        if (id != IID_IUnknown && id != IID_IEnumFORMATETC) return E_NOINTERFACE;
        *value = static_cast<IEnumFORMATETC*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC* values, ULONG* fetched) override {
        if (!values || (!fetched && count != 1)) return E_POINTER;
        ULONG done = 0; while (done < count && position_ < values_.size()) values[done++] = values_[position_++];
        if (fetched) *fetched = done; return done == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        const auto old = position_; position_ = (std::min)(values_.size(), position_ + count);
        return position_ - old == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { position_ = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** value) override {
        if (!value) return E_POINTER; *value = new FormatEnumerator(values_, position_); return S_OK;
    }
private:
    std::atomic_ulong references_{1}; std::vector<FORMATETC> values_; std::size_t position_ = 0;
};
struct DemandState {
    ClipboardDemandOffer offer;
    std::string operation = ClipboardCopyStore::new_id();
    std::function<void(ClipboardDemandEvent)> event;
    std::mutex mutex; std::condition_variable wake;
    std::shared_ptr<ClipboardDescription> description;
    std::shared_ptr<PreparedClipboardPayload> payload;
    std::filesystem::path directory;
    std::vector<std::uint64_t> prefix;
    std::vector<bool> read;
    bool described = false, requested = false, reading = false, approved = false, cancelled = false, reported = false;
    bool ended = false, end_ok = false;
    DWORD performed = 0, succeeded = 0, effect = 0;
    std::string error;
    void request(bool metadata) {
        ClipboardDemandEvent message;
        {
            std::lock_guard lock(mutex);
            auto& sent = metadata ? described : requested;
            if (sent || cancelled) return;
            sent = true; message.kind = metadata ? ClipboardDemandEventKind::kDescribe : ClipboardDemandEventKind::kDemand;
            message.operation = operation; message.offer = offer;
        }
        if (event) event(std::move(message));
    }
    void request_payload() {
        ClipboardDemandEvent message;
        { std::lock_guard lock(mutex); if (reading || cancelled) return; reading = true;
          message.kind = ClipboardDemandEventKind::kRead; message.operation = operation; message.offer = offer; }
        if (event) event(std::move(message));
    }
    template<class Ready> bool wait(Ready ready) {
        // COM callers may synchronously ask for a format. Pump this dedicated
        // STA, never the product GUI/runtime, so cancel/clipboard-loss can run.
        for (;;) {
            { std::unique_lock lock(mutex); if (cancelled) return false; if (ready()) return true;
              wake.wait_for(lock, std::chrono::milliseconds(20)); }
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
    }
    void cancel(std::string reason = "transfer_cancelled") {
        { std::lock_guard lock(mutex); cancelled = true; error = std::move(reason); } wake.notify_all();
    }
    void result(bool plain = false) {
        ClipboardDemandEvent message;
        {
            std::lock_guard lock(mutex);
            if (reported || cancelled) return;
            const bool files = (offer.formats & kClipboardFilesFormat) != 0;
            bool all = !files || (description && read.size() == description->entries.size());
            if (files && all) for (std::size_t i = 0; i < read.size(); ++i) {
                const auto& entry = description->entries[i];
                if (!entry.directory && (!read[i] || prefix[i] < entry.size)) { all = false; break; }
            }
            const bool move = performed == DROPEFFECT_MOVE && succeeded == DROPEFFECT_MOVE;
            const bool copy = succeeded == DROPEFFECT_COPY && performed != DROPEFFECT_MOVE;
            // EndOperation may precede the Shell's two move feedback formats.
            // A successful MOVE alone is not permission to delete the source.
            const bool finished = plain || move || copy || (ended && (!end_ok || effect != DROPEFFECT_MOVE || !offer.move));
            if (!finished) return;
            reported = true; message.kind = ClipboardDemandEventKind::kNativeResult; message.operation = operation;
            message.offer = offer; message.complete = all && (!ended || end_ok);
            message.effect = move ? 2U : effect == DROPEFFECT_MOVE ? 0U : 1U;
            if (!message.complete) message.error = "clipboard_native_paste_incomplete";
        }
        if (event) event(std::move(message));
    }
};
class DemandStream final : public IStream {
public:
    DemandStream(std::shared_ptr<DemandState> state, std::size_t index, std::uint64_t position = 0)
        : state_(std::move(state)), index_(index), position_(position) {}
    ~DemandStream() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override {
        if (!value) return E_POINTER; *value = nullptr;
        if (id != IID_IUnknown && id != IID_ISequentialStream && id != IID_IStream) return E_NOINTERFACE;
        *value = static_cast<IStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE Read(void* bytes, ULONG count, ULONG* read) override {
        if (read) *read = 0; if (!bytes && count) return STG_E_INVALIDPOINTER; if (!count) return S_OK;
        if (!ordinary_desktop()) return E_ACCESSDENIED;
        state_->request_payload();
        if (!state_->wait([&] { return bool(state_->payload); })) return E_ABORT;
        std::lock_guard file_lock(mutex_);
        ClipboardDescribedEntry entry;
        std::filesystem::path path;
        { std::lock_guard lock(state_->mutex); entry = state_->description->entries[index_];
          path = state_->directory / std::filesystem::path(wide(entry.staged_path)); }
        if (file_ == INVALID_HANDLE_VALUE) {
            file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            BY_HANDLE_FILE_INFORMATION info{};
            if (file_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file_, &info)
                || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
                || ((std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow) != entry.size) return STG_E_READFAULT;
        }
        LARGE_INTEGER offset{}; offset.QuadPart = static_cast<LONGLONG>(position_);
        if (!SetFilePointerEx(file_, offset, nullptr, FILE_BEGIN)) return STG_E_SEEKERROR;
        DWORD done = 0; if (!ReadFile(file_, bytes, count, &done, nullptr)) return STG_E_READFAULT;
        { std::lock_guard lock(state_->mutex); state_->read[index_] = true;
          if (position_ <= state_->prefix[index_]) state_->prefix[index_] = (std::max)(state_->prefix[index_], position_ + done); }
        position_ += done; if (read) *read = done; state_->result();
        return done == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
        std::lock_guard lock(mutex_); std::lock_guard state_lock(state_->mutex);
        if (origin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
        const auto base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? position_ : state_->description->entries[index_].size;
        if ((move.QuadPart < 0 && static_cast<std::uint64_t>(-(move.QuadPart + 1)) + 1 > base)
            || (move.QuadPart >= 0 && static_cast<std::uint64_t>(move.QuadPart) > UINT64_MAX - base)) return STG_E_INVALIDFUNCTION;
        position_ = move.QuadPart < 0 ? base - (static_cast<std::uint64_t>(-(move.QuadPart + 1)) + 1) : base + move.QuadPart;
        if (result) result->QuadPart = position_; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER amount, ULARGE_INTEGER* read, ULARGE_INTEGER* written) override {
        if (!target) return STG_E_INVALIDPOINTER;
        std::uint8_t buffer[64U * 1024U]; std::uint64_t total = 0;
        while (total < amount.QuadPart) {
            ULONG count = 0, done = 0; const auto status = Read(buffer, static_cast<ULONG>((std::min)(std::uint64_t(sizeof(buffer)), amount.QuadPart - total)), &count);
            if (FAILED(status)) return status;
            const auto sent = target->Write(buffer, count, &done); if (FAILED(sent) || done != count) return STG_E_WRITEFAULT;
            total += done; if (!count || status == S_FALSE) break;
        }
        if (read) read->QuadPart = total; if (written) written->QuadPart = total;
        return total == amount.QuadPart ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* value, DWORD) override {
        if (!value) return E_POINTER; *value = {}; value->type = STGTY_STREAM;
        std::lock_guard lock(state_->mutex); value->cbSize.QuadPart = state_->description->entries[index_].size;
        value->grfMode = STGM_READ; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** value) override {
        if (!value) return E_POINTER; std::lock_guard lock(mutex_);
        *value = new DemandStream(state_, index_, position_); return S_OK;
    }
private:
    std::atomic_ulong references_{1}; std::shared_ptr<DemandState> state_;
    std::size_t index_; std::uint64_t position_; HANDLE file_ = INVALID_HANDLE_VALUE; std::mutex mutex_;
};
class DemandObject final : public IDataObject, public IDataObjectAsyncCapability {
public:
    explicit DemandObject(std::shared_ptr<DemandState> state) : state_(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override {
        if (!value) return E_POINTER; *value = nullptr;
        if (id == IID_IUnknown || id == IID_IDataObject) *value = static_cast<IDataObject*>(this);
        else if (id == __uuidof(IDataObjectAsyncCapability)) *value = static_cast<IDataObjectAsyncCapability*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* value) override {
        if (!value) return E_POINTER;
        if (value->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        for (const auto& item : formats()) if (item.cfFormat == value->cfFormat && (item.tymed & value->tymed)) return S_OK;
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* value, STGMEDIUM* result) override {
        if (!result) return E_POINTER; *result = {};
        const auto available = QueryGetData(value); if (FAILED(available)) return available;
        const auto type = value->cfFormat;
        if (type == RegisterClipboardFormatW(kRemoteFormat) || type == RegisterClipboardFormatW(L"Preferred DropEffect")
            || type == RegisterClipboardFormatW(L"CanIncludeInClipboardHistory") || type == RegisterClipboardFormatW(L"CanUploadToCloudClipboard")) {
            DWORD flag = type == RegisterClipboardFormatW(kRemoteFormat) ? 5
                : type == RegisterClipboardFormatW(L"Preferred DropEffect") ? state_->offer.move ? DROPEFFECT_MOVE : DROPEFFECT_COPY : 0;
            return global_medium(result, &flag, sizeof(flag));
        }
        if (!ordinary_desktop()) return E_ACCESSDENIED;
        if (state_->offer.formats & kClipboardFilesFormat) {
            state_->request(true);
            if (!state_->wait([&] { return bool(state_->description); })) return E_ABORT;
            if (type == RegisterClipboardFormatW(L"FileContents")) {
                std::lock_guard lock(state_->mutex);
                if (value->lindex < 0 || static_cast<std::size_t>(value->lindex) >= state_->description->entries.size()
                    || state_->description->entries[value->lindex].directory) return DV_E_LINDEX;
                if (!state_->description->entries[value->lindex].size) state_->read[value->lindex] = true;
                result->tymed = TYMED_ISTREAM; result->pstm = new DemandStream(state_, value->lindex); return S_OK;
            }
            state_->request(false);
            if (!state_->wait([&] { return state_->approved; })) return E_ABORT;
            std::lock_guard lock(state_->mutex);
            const auto& entries = state_->description->entries;
            const auto bytes = offsetof(FILEGROUPDESCRIPTORW, fgd) + entries.size() * sizeof(FILEDESCRIPTORW);
            const auto status = global_medium(result, nullptr, bytes); if (FAILED(status)) return status;
            auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(result->hGlobal));
            group->cItems = static_cast<UINT>(entries.size());
            for (std::size_t i = 0; i < entries.size(); ++i) {
                auto name = wide(entries[i].relative_path); std::replace(name.begin(), name.end(), L'/', L'\\');
                if (name.empty() || name.size() >= MAX_PATH) { GlobalUnlock(result->hGlobal); ReleaseStgMedium(result); *result = {}; return DV_E_FORMATETC; }
                auto& entry = group->fgd[i]; entry.dwFlags = FD_ATTRIBUTES | FD_PROGRESSUI;
                entry.dwFileAttributes = entries[i].directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
                if (!entries[i].directory) { entry.dwFlags |= FD_FILESIZE; entry.nFileSizeHigh = static_cast<DWORD>(entries[i].size >> 32); entry.nFileSizeLow = static_cast<DWORD>(entries[i].size); }
                std::copy(name.begin(), name.end(), entry.cFileName); entry.cFileName[name.size()] = L'\0';
            }
            GlobalUnlock(result->hGlobal); return S_OK;
        }
        state_->request(false);
        if (!state_->wait([&] { return bool(state_->payload); })) return E_ABORT;
        std::shared_ptr<PreparedClipboardPayload> payload;
        { std::lock_guard lock(state_->mutex); payload = state_->payload; }
        for (std::uint32_t kind = 1; kind <= 6; ++kind) if (type == format(static_cast<ClipboardPayloadFormat>(kind))) {
            result->hGlobal = reinterpret_cast<HGLOBAL>(payload->clone_native_format(static_cast<ClipboardPayloadFormat>(kind)));
            if (!result->hGlobal) return DV_E_FORMATETC;
            result->tymed = TYMED_HGLOBAL; state_->result(true); return S_OK;
        }
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return DATA_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* result) override { if (result) result->ptd = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* value, STGMEDIUM* data, BOOL release) override {
        if (!value || !data || data->tymed != TYMED_HGLOBAL || !data->hGlobal || GlobalSize(data->hGlobal) < sizeof(DWORD)) return DV_E_FORMATETC;
        const auto* effect = static_cast<const DWORD*>(GlobalLock(data->hGlobal)); if (!effect) return E_INVALIDARG;
        bool known = true;
        { std::lock_guard lock(state_->mutex);
          if (value->cfFormat == RegisterClipboardFormatW(L"Performed DropEffect")) state_->performed = *effect;
          else if (value->cfFormat == RegisterClipboardFormatW(L"Paste Succeeded")) state_->succeeded = *effect;
          else known = false; }
        GlobalUnlock(data->hGlobal); if (!known) return DV_E_FORMATETC;
        if (release) ReleaseStgMedium(data);
        bool empty = false;
        { std::lock_guard lock(state_->mutex); empty = state_->description && !state_->description->totals.bytes; }
        if (empty) state_->request_payload();
        state_->result(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** result) override {
        if (!result) return E_POINTER; *result = nullptr; if (direction != DATADIR_GET) return E_NOTIMPL;
        *result = new FormatEnumerator(formats()); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL value) override { async_ = value; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* value) override { if (!value) return E_POINTER; *value = async_; return S_OK; }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override { in_operation_ = true; return S_OK; }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* value) override { if (!value) return E_POINTER; *value = in_operation_; return S_OK; }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT status, IBindCtx*, DWORD effect) override {
        in_operation_ = false;
        bool empty_folders = false;
        { std::lock_guard lock(state_->mutex); empty_folders = state_->description && !state_->description->totals.bytes; }
        if (SUCCEEDED(status) && empty_folders) state_->request_payload();
        { std::lock_guard lock(state_->mutex); state_->ended = true; state_->end_ok = SUCCEEDED(status); state_->effect = effect; }
        state_->result(); return S_OK;
    }
private:
    std::vector<FORMATETC> formats() const {
        std::vector<FORMATETC> result;
        const auto add = [&](UINT type, DWORD medium = TYMED_HGLOBAL, LONG index = -1) {
            result.push_back({static_cast<CLIPFORMAT>(type), nullptr, DVASPECT_CONTENT, index, medium});
        };
        add(RegisterClipboardFormatW(kRemoteFormat)); add(RegisterClipboardFormatW(L"CanIncludeInClipboardHistory"));
        add(RegisterClipboardFormatW(L"CanUploadToCloudClipboard"));
        if (state_->offer.formats & kClipboardFilesFormat) {
            add(RegisterClipboardFormatW(L"FileGroupDescriptorW")); add(RegisterClipboardFormatW(L"FileContents"), TYMED_ISTREAM, 0);
            add(RegisterClipboardFormatW(L"Preferred DropEffect"));
        } else for (std::uint32_t kind : {2U, 3U, 4U, 5U, 6U, 1U})
            if (state_->offer.formats & (1U << (kind - 1))) add(format(static_cast<ClipboardPayloadFormat>(kind)));
        return result;
    }
    std::atomic_ulong references_{1}; std::shared_ptr<DemandState> state_; BOOL async_ = TRUE, in_operation_ = FALSE;
};
struct PortShared {
    std::mutex mutex;
    std::deque<std::function<void()>> commands;
    std::deque<ClipboardDemandEvent> events;
    ClipboardDemandOffer source;
    std::vector<ClipboardFileSelection> files;
    std::atomic_bool stopping{false};
    bool failure_reported = false;
    HWND window = nullptr;
    void emit(ClipboardDemandEvent event) {
        std::lock_guard lock(mutex);
        if (event.kind == ClipboardDemandEventKind::kOffer) {
            for (auto& existing : events) if (existing.kind == event.kind) { existing = std::move(event); return; }
        }
        if (events.size() < 16) events.push_back(std::move(event));
        else { stopping = true; if (window) PostMessageW(window, WM_APP, 0, 0); }
    }
};
class NativePort final : public ClipboardDemandPort {
public:
    NativePort() : shared_(std::make_shared<PortShared>()), thread_([this] { run(); }) {}
    ~NativePort() override {
        shared_->stopping = true;
        { std::lock_guard lock(shared_->mutex); if (shared_->window) PostMessageW(shared_->window, WM_APP, 0, 0); }
        if (thread_.joinable()) thread_.join();
    }
    void connected(bool enabled, std::string epoch) override {
        { std::lock_guard lock(configuration_mutex_); if (enabled == configured_ && epoch == configured_epoch_) return;
          configured_ = enabled; configured_epoch_ = epoch; }
        post([this, enabled, epoch = std::move(epoch)] {
            if (enabled_ != enabled || epoch_ != epoch) {
                if (state_) state_->cancel("clipboard_session_changed");
                if (proxy_ && OleIsCurrentClipboard(proxy_.Get()) == S_OK) OleSetClipboard(nullptr);
                proxy_.Reset(); state_.reset(); original_.Reset();
                { std::lock_guard lock(shared_->mutex); shared_->source = {}; shared_->files.clear(); shared_->events.clear(); }
            }
            enabled_ = enabled; epoch_ = epoch; observed_ = GetClipboardSequenceNumber();
        });
    }
    std::optional<ClipboardDemandEvent> take_event() override {
        std::lock_guard lock(shared_->mutex);
        if (shared_->stopping && !shared_->failure_reported) {
            shared_->failure_reported = true; shared_->events.clear();
            return ClipboardDemandEvent{ClipboardDemandEventKind::kUnavailable};
        }
        if (shared_->events.empty()) return {};
        auto event = std::move(shared_->events.front()); shared_->events.pop_front(); return event;
    }
    void remote_offer(ClipboardDemandOffer offer, std::string invoke) override {
        post([this, offer = std::move(offer), invoke = std::move(invoke)] {
            if (!enabled_ || !ordinary_desktop()) return;
            if (state_) state_->cancel("clipboard_source_changed");
            if (!offer.formats) {
                if (proxy_ && OleIsCurrentClipboard(proxy_.Get()) == S_OK) OleSetClipboard(nullptr);
                observed_ = GetClipboardSequenceNumber(); proxy_.Reset(); state_.reset(); return;
            }
            state_ = std::make_shared<DemandState>(); state_->offer = offer;
            if (!invoke.empty()) state_->operation = invoke;
            std::weak_ptr<PortShared> weak = shared_;
            state_->event = [weak](auto event) { if (auto shared = weak.lock()) shared->emit(std::move(event)); };
            proxy_.Attach(new DemandObject(state_));
            if (FAILED(OleSetClipboard(proxy_.Get()))) { state_->cancel("clipboard_busy"); return; }
            observed_ = GetClipboardSequenceNumber();
            if (!invoke.empty()) shared_->emit({ClipboardDemandEventKind::kInstalled, invoke, offer});
        });
    }
    void refresh_for_shortcut(std::string operation, std::uint32_t sequence) override {
        post([this, operation = std::move(operation), sequence] {
            if (!enabled_ || !sequence || sequence != GetClipboardSequenceNumber() || !capture()) {
                shared_->emit({ClipboardDemandEventKind::kShortcutReady, operation, {}, false, 0, "clipboard_source_changed"}); return;
            }
            ClipboardDemandOffer offer;
            { std::lock_guard lock(shared_->mutex); offer = shared_->source; }
            shared_->emit({ClipboardDemandEventKind::kShortcutReady, operation, offer});
        });
    }
    bool source(std::string_view token, std::uint32_t& sequence, std::vector<ClipboardFileSelection>& files) override {
        std::lock_guard lock(shared_->mutex);
        if (shared_->source.token != token || !shared_->source.sequence || shared_->source.sequence != GetClipboardSequenceNumber()) return false;
        sequence = shared_->source.sequence; files = shared_->files; return true;
    }
    void description(std::string operation, std::shared_ptr<ClipboardDescription> description, std::string error) override {
        post([this, operation = std::move(operation), description = std::move(description), error = std::move(error)] {
            if (!state_ || state_->operation != operation) return;
            if (!description || !error.empty()) { state_->cancel(error); return; }
            { std::lock_guard lock(state_->mutex); state_->description = description;
              state_->prefix.resize(description->entries.size()); state_->read.resize(description->entries.size()); }
            state_->wake.notify_all();
        });
    }
    void approve(std::string operation) override {
        post([this, operation = std::move(operation)] {
            if (state_ && state_->operation == operation) {
                { std::lock_guard lock(state_->mutex); state_->approved = true; } state_->wake.notify_all();
            }
        });
    }
    void payload(std::string operation, std::shared_ptr<PreparedClipboardPayload> payload, std::filesystem::path directory, std::string error) override {
        post([this, operation = std::move(operation), payload = std::move(payload), directory = std::move(directory), error = std::move(error)] {
            if (!state_ || state_->operation != operation) return;
            if (!payload || !error.empty()) { state_->cancel(error); return; }
            { std::lock_guard lock(state_->mutex); state_->payload = payload; state_->directory = directory; }
            state_->wake.notify_all();
        });
    }
    void cancel(std::string operation) override {
        post([this, operation = std::move(operation)] {
            if (state_ && (operation.empty() || state_->operation == operation)) {
                state_->cancel();
                if (enabled_ && proxy_ && OleIsCurrentClipboard(proxy_.Get()) == S_OK) remote_offer(state_->offer, {});
            }
        });
    }
    void finished(std::string operation, bool moved) override {
        if (!moved) return;
        post([this, operation = std::move(operation)] {
            if (state_ && state_->operation == operation) {
                state_->cancel("clipboard_move_completed");
                if (proxy_ && OleIsCurrentClipboard(proxy_.Get()) == S_OK) OleSetClipboard(nullptr);
                observed_ = GetClipboardSequenceNumber(); proxy_.Reset(); state_.reset();
            }
        });
    }
    void commit_source(std::string operation, std::string token, std::shared_ptr<ClipboardDescription> description) override {
        post([this, operation = std::move(operation), token = std::move(token), description = std::move(description)] {
            ClipboardDemandOffer offer; { std::lock_guard lock(shared_->mutex); offer = shared_->source; }
            std::string error; const std::atomic_bool cancelled{false};
            bool feedback_started = false;
            bool ok = original_ && offer.token == token && offer.move && offer.sequence == GetClipboardSequenceNumber()
                && ordinary_desktop() && description && description->sources_unchanged(cancelled, &error);
            if (ok && committed_ != operation) {
                committed_ = operation;
                auto original = original_;
                committing_source_ = true;
                feedback_started = true;
                DWORD effect = DROPEFFECT_MOVE;
                for (const auto* name : {L"Performed DropEffect", L"Paste Succeeded"}) {
                    FORMATETC type{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                    STGMEDIUM data{};
                    ok = SUCCEEDED(global_medium(&data, &effect, sizeof(effect)));
                    if (ok) { ok = SUCCEEDED(original->SetData(&type, &data, FALSE)); ReleaseStgMedium(&data); }
                    if (!ok) break;
                }
                // The original source owns deletion. Never delete paths here.
                if (ok) for (const auto& file : description->identities)
                    if (GetFileAttributesW(file.path.c_str()) != INVALID_FILE_ATTRIBUTES) { ok = false; break; }
                observed_ = GetClipboardSequenceNumber(); committing_source_ = false;
            } else if (committed_ == operation) { ok = committed_ok_; error = committed_error_; }
            if (!ok && error.empty()) error = feedback_started ? "clipboard_move_result_unknown" : "clipboard_source_retained";
            committed_ok_ = ok;
            committed_error_ = error;
            shared_->emit({ClipboardDemandEventKind::kMoveReceipt, operation, offer, ok, ok ? 2U : 0U,
                ok ? "" : error});
        });
    }
private:
    void post(std::function<void()> command) {
        std::lock_guard lock(shared_->mutex);
        if (shared_->stopping) return;
        if (shared_->commands.size() >= 16) { shared_->stopping = true; }
        else shared_->commands.push_back(std::move(command));
        if (shared_->window) PostMessageW(shared_->window, WM_APP, 0, 0);
    }
    bool capture() {
        if (!ordinary_desktop() || (proxy_ && OleIsCurrentClipboard(proxy_.Get()) == S_OK)) return false;
        ComPtr<IDataObject> data; if (FAILED(OleGetClipboard(&data))) return false;
        ClipboardDemandOffer offer; offer.token = ClipboardCopyStore::new_id(); offer.sequence = GetClipboardSequenceNumber();
        std::vector<ClipboardFileSelection> files;
        const auto supports = [&](UINT type) { FORMATETC request{static_cast<CLIPFORMAT>(type), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; return data->QueryGetData(&request) == S_OK; };
        for (std::uint32_t kind = 1; kind <= 6; ++kind)
            if (supports(format(static_cast<ClipboardPayloadFormat>(kind)))) offer.formats |= 1U << (kind - 1);
        if (supports(CF_HDROP)) {
            FORMATETC request{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; STGMEDIUM medium{};
            if (FAILED(data->GetData(&request, &medium))) return false;
            const auto count = DragQueryFileW(static_cast<HDROP>(medium.hGlobal), 0xffffffff, nullptr, 0);
            if (count > 1024) { ReleaseStgMedium(&medium); return false; }
            for (UINT i = 0; i < count; ++i) {
                const auto length = DragQueryFileW(static_cast<HDROP>(medium.hGlobal), i, nullptr, 0);
                if (!length || length > 32760) { ReleaseStgMedium(&medium); return false; }
                std::wstring path(length + 1, L'\0'); DragQueryFileW(static_cast<HDROP>(medium.hGlobal), i, path.data(), length + 1); path.resize(length);
                const std::filesystem::path source(path); const auto name = source.filename().generic_u8string();
                if (!source.is_absolute()) { ReleaseStgMedium(&medium); return false; }
                files.push_back({source, "files/" + std::to_string(i) + "/" + std::string(reinterpret_cast<const char*>(name.data()), name.size())});
            }
            ReleaseStgMedium(&medium); if (!files.empty()) offer.formats = kClipboardFilesFormat;
            FORMATETC effect{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Preferred DropEffect")), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            if (SUCCEEDED(data->GetData(&effect, &medium))) {
                if (medium.tymed == TYMED_HGLOBAL && GlobalSize(medium.hGlobal) >= sizeof(DWORD)) {
                    const auto* value = static_cast<const DWORD*>(GlobalLock(medium.hGlobal));
                    if (value) { offer.move = *value == DROPEFFECT_MOVE; GlobalUnlock(medium.hGlobal); }
                }
                ReleaseStgMedium(&medium);
            }
        }
        if (offer.sequence != GetClipboardSequenceNumber()) return false;
        original_ = data;
        { std::lock_guard lock(shared_->mutex); shared_->source = offer; shared_->files = std::move(files); }
        shared_->emit({ClipboardDemandEventKind::kOffer, {}, offer}); return true;
    }
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<NativePort*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<NativePort*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        if (self && message == WM_CLIPBOARDUPDATE) {
            const auto sequence = GetClipboardSequenceNumber();
            if (self->committing_source_) { self->observed_ = sequence; return 0; }
            if (self->enabled_ && sequence != self->observed_) {
                self->observed_ = sequence;
                if (!self->proxy_ || OleIsCurrentClipboard(self->proxy_.Get()) != S_OK) {
                    if (self->state_) self->state_->cancel("clipboard_local_changed");
                    try { self->capture(); } catch (...) { self->shared_->stopping = true; self->drain(); }
                }
            }
            return 0;
        }
        if (self && message == WM_APP) { self->drain(); return 0; }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    void drain() {
        for (unsigned i = 0; i < 16; ++i) {
            std::function<void()> command;
            { std::lock_guard lock(shared_->mutex); if (shared_->commands.empty()) break;
              command = std::move(shared_->commands.front()); shared_->commands.pop_front(); }
            try { command(); } catch (...) { shared_->stopping = true; }
        }
        if (shared_->stopping && state_) state_->cancel("clipboard_session_closed");
    }
    void run() {
        const auto initialized = OleInitialize(nullptr);
        if (FAILED(initialized)) { shared_->stopping = true; return; }
        const wchar_t* name = L"RedClawClipboardDemandV5";
        WNDCLASSW type{}; type.lpfnWndProc = window_proc; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = name;
        RegisterClassW(&type);
        const auto window = CreateWindowExW(0, name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, this);
        { std::lock_guard lock(shared_->mutex); shared_->window = window; }
        if (window) {
            if (!AddClipboardFormatListener(window)) shared_->stopping = true;
            observed_ = GetClipboardSequenceNumber(); drain();
            while (!shared_->stopping) {
                MSG message{};
                if (GetMessageW(&message, nullptr, 0, 0) <= 0) break;
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            if (state_) state_->cancel("clipboard_session_closed");
            if (proxy_ && OleIsCurrentClipboard(proxy_.Get()) == S_OK) OleSetClipboard(nullptr);
            RemoveClipboardFormatListener(window); DestroyWindow(window);
        } else shared_->stopping = true;
        proxy_.Reset(); original_.Reset(); state_.reset();
        { std::lock_guard lock(shared_->mutex); shared_->window = nullptr; shared_->commands.clear(); }
        OleUninitialize();
    }
    std::shared_ptr<PortShared> shared_;
    ComPtr<IDataObject> proxy_, original_;
    std::shared_ptr<DemandState> state_;
    bool enabled_ = false, configured_ = false, committed_ok_ = false, committing_source_ = false;
    std::uint32_t observed_ = 0;
    std::string epoch_, configured_epoch_, committed_, committed_error_;
    std::mutex configuration_mutex_;
    std::thread thread_;
};
}
#endif
std::shared_ptr<ClipboardDemandPort> make_native_clipboard_demand_port() {
#ifdef _WIN32
    return std::make_shared<NativePort>();
#else
    return {};
#endif
}
bool clipboard_is_remote_offer() {
#ifdef _WIN32
    return IsClipboardFormatAvailable(RegisterClipboardFormatW(kRemoteFormat)) != FALSE;
#else
    return false;
#endif
}
bool clipboard_has_files() {
#ifdef _WIN32
    return IsClipboardFormatAvailable(CF_HDROP) != FALSE;
#else
    return false;
#endif
}
void* make_clipboard_data_object_for_test(ClipboardDemandOffer offer, std::function<void(ClipboardDemandEvent)> event,
    std::function<void(std::shared_ptr<PreparedClipboardPayload>, std::shared_ptr<ClipboardDescription>, std::filesystem::path)>* supply) {
#ifdef _WIN32
    auto state = std::make_shared<DemandState>(); state->offer = std::move(offer); state->event = std::move(event);
    if (supply) *supply = [state](auto payload, auto description, auto directory) {
        { std::lock_guard lock(state->mutex); state->payload = std::move(payload); state->description = std::move(description);
          state->directory = std::move(directory);
          state->approved = true; if (state->description) { state->prefix.resize(state->description->entries.size()); state->read.resize(state->description->entries.size()); } }
        state->wake.notify_all();
    };
    return static_cast<IDataObject*>(new DemandObject(std::move(state)));
#else
    (void)offer; (void)event; (void)supply; return nullptr;
#endif
}
}
