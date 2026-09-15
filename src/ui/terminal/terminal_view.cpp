// The SDK checks whether __has_attribute is defined, while Qt supplies a zero
// fallback on MSVC. Parse the native SDK before that Qt compatibility macro.
#define NOMINMAX
#include <Windows.h>
#include <Shlwapi.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wrl.h>

#include "ui/terminal/terminal_view.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QResizeEvent>
#include <QResource>
#include <QUrl>


static void initialize_terminal_resources() { Q_INIT_RESOURCE(terminal_assets); }

namespace redclaw::ui {
namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
constexpr auto kOrigin = "https://redclaw-terminal.invalid/";
constexpr auto kPage = "https://redclaw-terminal.invalid/index.html";
constexpr std::size_t kOutputChunkBytes = 16 * 1024;
constexpr int kMaxBridgeCharacters = 96 * 1024;

QString take_string(LPWSTR value) {
    const auto result = QString::fromWCharArray(value ? value : L"");
    CoTaskMemFree(value);
    return result;
}

void deny_navigation(ICoreWebView2NavigationStartingEventArgs* args, bool allow_page) {
    LPWSTR value = nullptr;
    args->get_Uri(&value);
    const auto uri = take_string(value);
    if (!allow_page || uri != QLatin1String(kPage)) args->put_Cancel(TRUE);
}
}

struct TerminalView::Impl {
    TerminalView* owner;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    std::function<void(const QByteArray&)> input;
    std::function<void(int, int)> resized;
    std::function<void()> writable;
    std::function<void(const QString&)> error;
    QString epoch;
    std::uint64_t sequence = 0;
    bool initialized = false, com_owned = false, page_ready = false, session_ready = false;
    bool input_enabled = false, pending = false;

    void failure(const char* stage, HRESULT result) {
        page_ready = false; session_ready = false; pending = false;
        if (error) error(QStringLiteral("terminal_%1:0x%2").arg(QLatin1String(stage))
            .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0')));
    }
    void post(QJsonObject value) {
        if (!webview || !page_ready) return;
        value.insert("v", 1); value.insert("epoch", epoch);
        const auto json = QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)).toStdWString();
        const auto result = webview->PostWebMessageAsJson(json.c_str());
        if (FAILED(result)) failure("post", result);
    }
    void bounds() {
        if (!controller) return;
        const auto ratio = owner->devicePixelRatioF();
        RECT rect{0, 0, qRound(owner->width() * ratio), qRound(owner->height() * ratio)};
        controller->put_Bounds(rect);
        controller->put_IsVisible(owner->isVisible() ? TRUE : FALSE);
    }
    void message(ICoreWebView2WebMessageReceivedEventArgs* args) {
        LPWSTR source = nullptr;
        if (FAILED(args->get_Source(&source))) return;
        if (take_string(source) != QLatin1String(kPage)) return;
        LPWSTR json = nullptr;
        if (FAILED(args->get_WebMessageAsJson(&json))) return;
        const auto text = take_string(json);
        if (text.size() > kMaxBridgeCharacters) return;
        const auto document = QJsonDocument::fromJson(text.toUtf8());
        if (!document.isObject()) return;
        const auto object = document.object();
        if (object.value("v").toInt() != 1) return;
        const auto type = object.value("type").toString();
        if (type == "ready" && !page_ready) {
            page_ready = true;
            if (!epoch.isEmpty()) {
                post({{"type", "reset"}});
            }
            return;
        }
        if (!page_ready || epoch.isEmpty() || object.value("epoch").toString() != epoch) return;
        if (type == "reset_done" && !session_ready) {
            session_ready = true;
            post({{"type", "enabled"}, {"enabled", input_enabled}});
            if (owner->hasFocus()) post({{"type", "focus"}});
            if (writable) writable();
            return;
        }
        if (!session_ready) return;
        if (type == "ack") {
            if (pending && object.value("sequence").toDouble() == static_cast<double>(sequence)) {
                pending = false;
                if (writable) writable();
            }
        } else if (type == "resize") {
            const int columns = object.value("cols").toInt(), rows = object.value("rows").toInt();
            if (columns >= 2 && rows >= 2 && columns <= 32767 && rows <= 32767 && resized) resized(columns, rows);
        } else if (input_enabled && (type == "input" || type == "binary")) {
            if (!object.value("data").isString()) return;
            auto bytes = object.value("data").toString().toUtf8();
            if (type == "binary") {
                const auto decoded = QByteArray::fromBase64Encoding(bytes, QByteArray::AbortOnBase64DecodingErrors);
                if (!decoded) return;
                bytes = decoded.decoded;
            }
            if (!bytes.isEmpty() && bytes.size() <= 64 * 1024 && input) input(bytes);
        }
    }
    void resource(ICoreWebView2WebResourceRequestedEventArgs* args) {
        ComPtr<ICoreWebView2WebResourceRequest> request;
        if (FAILED(args->get_Request(&request))) return;
        LPWSTR raw_uri = nullptr, raw_method = nullptr;
        request->get_Uri(&raw_uri); request->get_Method(&raw_method);
        const auto uri = take_string(raw_uri), method = take_string(raw_method);
        const auto relative = uri.startsWith(QLatin1String(kOrigin)) ? uri.mid(qstrlen(kOrigin)) : QString();
        const QStringList allowed{"index.html", "terminal.js", "terminal.css", "xterm.js", "xterm.css", "fit.js"};
        QByteArray bytes;
        QString mime;
        bool found = false;
        if (method == "GET" && allowed.contains(relative)) {
            QFile file(QStringLiteral(":/terminal/") + relative);
            found = file.open(QIODevice::ReadOnly);
            if (found) bytes = file.readAll();
            mime = relative.endsWith(".js") ? "application/javascript" : relative.endsWith(".css") ? "text/css" : "text/html";
        }
        ComPtr<IStream> stream;
        if (found) stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(bytes.constData()), static_cast<UINT>(bytes.size())));
        ComPtr<ICoreWebView2WebResourceResponse> response;
        const auto headers = QStringLiteral("Content-Type: %1; charset=utf-8\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff")
            .arg(found ? mime : "text/plain").toStdWString();
        if (SUCCEEDED(environment->CreateWebResourceResponse(stream.Get(), found ? 200 : 403,
            found ? L"OK" : L"Blocked", headers.c_str(), &response))) args->put_Response(response.Get());
    }
    void attach(ICoreWebView2Controller* created) {
        controller = created;
        HRESULT result = controller->get_CoreWebView2(&webview);
        if (FAILED(result)) { failure("webview", result); return; }
        HRESULT protection_error = S_OK;
        const auto protect = [&](HRESULT status) {
            if (FAILED(status) && SUCCEEDED(protection_error)) protection_error = status;
        };
        ComPtr<ICoreWebView2Settings> settings;
        webview->get_Settings(&settings);
        if (!settings) { failure("settings", E_FAIL); return; }
        protect(settings->put_IsWebMessageEnabled(TRUE));
        protect(settings->put_AreHostObjectsAllowed(FALSE));
        protect(settings->put_AreDevToolsEnabled(FALSE));
        protect(settings->put_AreDefaultContextMenusEnabled(FALSE));
        protect(settings->put_IsStatusBarEnabled(FALSE));
        protect(settings->put_AreDefaultScriptDialogsEnabled(FALSE));
        ComPtr<ICoreWebView2Settings3> settings3;
        protect(settings.As(&settings3));
        if (settings3) protect(settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE));
        EventRegistrationToken token{};
        QPointer<TerminalView> weak(owner);
        protect(webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                if (weak) weak->impl_->message(args); return S_OK;
            }).Get(), &token));
        protect(webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL));
        protect(webview->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                if (weak) weak->impl_->resource(args); return S_OK;
            }).Get(), &token));
        protect(webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                deny_navigation(args, true); return S_OK;
            }).Get(), &token));
        protect(webview->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                deny_navigation(args, false); return S_OK;
            }).Get(), &token));
        protect(webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE); return S_OK;
            }).Get(), &token));
        protect(webview->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY); return S_OK;
            }).Get(), &token));
        ComPtr<ICoreWebView2_4> webview4;
        protect(webview.As(&webview4));
        if (webview4) {
            protect(webview4->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>(
                [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                    args->put_Cancel(TRUE); return S_OK;
                }).Get(), &token));
        }
        protect(webview->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
                if (weak) weak->impl_->failure("renderer_exited", E_FAIL); return S_OK;
            }).Get(), &token));
        if (FAILED(protection_error)) { failure("surface_protection", protection_error); return; }
        bounds();
        result = webview->Navigate(L"https://redclaw-terminal.invalid/index.html");
        if (FAILED(result)) failure("navigate", result);
    }
};

TerminalView::TerminalView(QWidget* parent) : QWidget(parent), impl_(std::make_unique<Impl>()) {
    initialize_terminal_resources();
    impl_->owner = this;
    setObjectName("remoteTerminalView");
    setAttribute(Qt::WA_NativeWindow);
    setFocusPolicy(Qt::StrongFocus);
}
TerminalView::~TerminalView() {
    if (impl_->controller) impl_->controller->Close();
    impl_->webview.Reset(); impl_->controller.Reset(); impl_->environment.Reset();
    if (impl_->com_owned) CoUninitialize();
}
void TerminalView::initialize(const QString& runtime, const QString& user_data) {
    if (impl_->initialized) return;
    if (!QFileInfo(QDir(runtime).filePath("msedgewebview2.exe")).isFile() || !QDir().mkpath(user_data)) {
        impl_->failure("runtime_or_profile_unavailable", HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)); return;
    }
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) { impl_->failure("com_initialize", com); return; }
    impl_->com_owned = true;
    impl_->initialized = true;
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    options->put_AdditionalBrowserArguments(L"--disable-background-networking --disable-component-update --disable-sync --no-first-run");
    const auto runtime_path = QDir::toNativeSeparators(QFileInfo(runtime).absoluteFilePath()).toStdWString();
    const auto profile_path = QDir::toNativeSeparators(QFileInfo(user_data).absoluteFilePath()).toStdWString();
    QPointer<TerminalView> weak(this);
    const auto result = CreateCoreWebView2EnvironmentWithOptions(runtime_path.c_str(), profile_path.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [weak](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (!weak) return S_OK;
                if (FAILED(result) || !environment) { weak->impl_->failure("environment", result); return S_OK; }
                weak->impl_->environment = environment;
                const auto created = environment->CreateCoreWebView2Controller(reinterpret_cast<HWND>(weak->winId()),
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [weak](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (!weak) { if (controller) controller->Close(); return S_OK; }
                            if (FAILED(result) || !controller) weak->impl_->failure("controller", result);
                            else weak->impl_->attach(controller);
                            return S_OK;
                        }).Get());
                if (FAILED(created)) weak->impl_->failure("controller_create", created);
                return S_OK;
            }).Get());
    if (FAILED(result)) impl_->failure("environment_create", result);
}
void TerminalView::reset_session(const QString& epoch) {
    impl_->epoch = epoch; impl_->pending = false; impl_->input_enabled = false; impl_->session_ready = false;
    impl_->post({{"type", "reset"}});
}
void TerminalView::set_input_enabled(bool enabled) {
    if (impl_->input_enabled == enabled) return;
    impl_->input_enabled = enabled;
    if (impl_->session_ready) impl_->post({{"type", "enabled"}, {"enabled", enabled}});
}
bool TerminalView::append_output(std::string_view bytes) {
    if (!impl_->session_ready || !impl_->page_ready || impl_->pending || impl_->epoch.isEmpty() || bytes.empty() || bytes.size() > kOutputChunkBytes) return false;
    impl_->pending = true;
    ++impl_->sequence;
    const auto base64 = QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())).toBase64();
    impl_->post({{"type", "output"}, {"sequence", static_cast<double>(impl_->sequence)}, {"data", QString::fromLatin1(base64)}});
    return impl_->page_ready;
}
bool TerminalView::ready() const { return impl_->page_ready && impl_->session_ready; }
bool TerminalView::output_pending() const { return impl_->pending; }
void TerminalView::set_input_callback(std::function<void(const QByteArray&)> callback) { impl_->input = std::move(callback); }
void TerminalView::set_resize_callback(std::function<void(int, int)> callback) { impl_->resized = std::move(callback); }
void TerminalView::set_writable_callback(std::function<void()> callback) { impl_->writable = std::move(callback); }
void TerminalView::set_error_callback(std::function<void(const QString&)> callback) { impl_->error = std::move(callback); }
void TerminalView::resizeEvent(QResizeEvent* event) { QWidget::resizeEvent(event); impl_->bounds(); }
void TerminalView::showEvent(QShowEvent* event) { QWidget::showEvent(event); impl_->bounds(); }
void TerminalView::hideEvent(QHideEvent* event) { QWidget::hideEvent(event); impl_->bounds(); }
void TerminalView::focusInEvent(QFocusEvent* event) {
    QWidget::focusInEvent(event);
    if (impl_->controller) impl_->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    if (impl_->session_ready) impl_->post({{"type", "focus"}});
}
}
