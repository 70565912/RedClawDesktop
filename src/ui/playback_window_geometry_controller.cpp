#include "ui/playback_window_geometry_controller.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QColor>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include "ui/playback_geometry_transaction.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <dwmapi.h>
#endif

namespace redclaw::ui {
namespace {

class ResizeOutlineWidget final : public QWidget {
public:
    ResizeOutlineWidget()
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint
                    | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus) {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(QColor(34, 211, 238), 3));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(rect().adjusted(2, 2, -2, -2));
    }
};

}  // namespace

class PlaybackWindowGeometryController::Impl {
public:
    Impl(QWidget* playback_window, QWidget* playback_canvas)
        : playback_window_(playback_window), playback_canvas_(playback_canvas) {}

    void start(PlaybackWindowGeometryController* filter) {
        if (started_ || playback_window_.isNull() || playback_canvas_.isNull()) {
            return;
        }
        started_ = true;
        ++lifecycle_generation_;
        filter_ = filter;
        playback_window_->winId();
#if defined(_WIN32)
        const BOOL disabled = TRUE;
        const HRESULT transition_result = DwmSetWindowAttribute(
            reinterpret_cast<HWND>(playback_window_->winId()),
            DWMWA_TRANSITIONS_FORCEDISABLED,
            &disabled,
            sizeof(disabled));
        transitions_disabled_ = SUCCEEDED(transition_result);
#else
        transitions_disabled_ = false;
#endif
        QCoreApplication::instance()->installNativeEventFilter(filter_);
    }

    void stop() {
        if (!started_) {
            return;
        }
        started_ = false;
        ++lifecycle_generation_;
        commit_queued_ = false;
        if (QCoreApplication::instance() != nullptr && filter_ != nullptr) {
            QCoreApplication::instance()->removeNativeEventFilter(filter_);
        }
        if (outline_ != nullptr) {
            outline_->hide();
            outline_.reset();
        }
        set_active(false);
        filter_ = nullptr;
    }

    void set_active_callback(ActiveChangedCallback callback) {
        active_callback_ = std::move(callback);
    }

    void set_commit_callback(ViewportCommittedCallback callback) {
        commit_callback_ = std::move(callback);
    }

    void publish_initial_viewport() {
        schedule_viewport_commit(false);
    }

    QSize physical_canvas_size() const {
        if (playback_canvas_.isNull() || playback_canvas_->isHidden()) {
            return {};
        }
        return QSize(
            qRound(playback_canvas_->width() * playback_canvas_->devicePixelRatioF()),
            qRound(playback_canvas_->height() * playback_canvas_->devicePixelRatioF()));
    }

    void schedule_viewport_commit(bool geometry_commit) {
        if (!started_ || commit_queued_ || playback_window_.isNull() || filter_ == nullptr) {
            return;
        }
        commit_queued_ = true;
        const auto generation = lifecycle_generation_;
        // The callback owns Impl only as long as the controller exists, not as
        // long as its parent window exists. A stop/start also invalidates it.
        QTimer::singleShot(0, filter_, [this, geometry_commit, generation]() {
            if (!started_ || generation != lifecycle_generation_) return;
            commit_queued_ = false;
            if (playback_window_.isNull() || playback_window_->isMinimized()) {
                set_active(false);
                return;
            }
            if (geometry_commit) {
                ++geometry_commit_total_;
            }
            set_active(false);
            const QSize viewport = physical_canvas_size();
            if (viewport.width() < 64 || viewport.height() < 64 || viewport == last_viewport_) {
                return;
            }
            last_viewport_ = viewport;
            ++viewport_commit_total_;
            if (commit_callback_) {
                commit_callback_(viewport, ++transaction_id_);
            }
        });
    }

    void set_active(bool active) {
        if (transaction_active_ == active) {
            return;
        }
        transaction_active_ = active;
        if (active_callback_) {
            active_callback_(active);
        }
    }

#if defined(_WIN32)
    static PlaybackWindowRect from_rect(const RECT& rect) {
        return {rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top};
    }

    static RECT to_rect(const PlaybackWindowRect& rect) {
        return {rect.x, rect.y, rect.x + rect.width, rect.y + rect.height};
    }

    bool handle_windows_message(MSG* event, qintptr* result) {
        // winId() is a creating accessor. During DestroyWindow/Qt teardown it
        // recreates the HWND and reenters this native filter recursively (seen
        // in real close dumps ending in TextShaping/IME stack exhaustion).
        // Only inspect the currently existing handle; never create one here.
        if (!started_ || event == nullptr || event->hwnd == nullptr || playback_window_.isNull()
            || event->hwnd != reinterpret_cast<HWND>(playback_window_->internalWinId())) {
            return false;
        }

        switch (event->message) {
        case WM_ENTERSIZEMOVE: {
            RECT current{};
            if (GetWindowRect(event->hwnd, &current) != FALSE) {
                transaction_.begin(from_rect(current));
                interactive_loop_ = true;
                sizing_active_ = false;
                cancelled_ = false;
            }
            return false;
        }
        case WM_SIZING: {
            if (!interactive_loop_ || event->lParam == 0) {
                return false;
            }
            auto* proposed = reinterpret_cast<RECT*>(event->lParam);
            if (!transaction_.preview(from_rect(*proposed))) {
                return false;
            }
            ++geometry_preview_total_;
            sizing_active_ = true;
            set_active(true);
            show_outline(*proposed);
            *proposed = to_rect(transaction_.committed_rect());
            if (result != nullptr) {
                *result = TRUE;
            }
            return true;
        }
        case WM_WINDOWPOSCHANGING: {
            if (applying_final_rect_ || event->lParam == 0) {
                return false;
            }
            auto* position = reinterpret_cast<WINDOWPOS*>(event->lParam);
            if (!sizing_active_) {
                if ((position->flags & SWP_NOSIZE) == 0) {
                    RECT current{};
                    const bool size_changes = GetWindowRect(event->hwnd, &current) != FALSE
                        && position->cx > 0 && position->cy > 0
                        && (position->cx != current.right - current.left
                            || position->cy != current.bottom - current.top);
                    if (size_changes) {
                        set_active(true);
                        state_change_pending_ = true;
                    }
                }
                return false;
            }
            const PlaybackWindowRect committed = transaction_.committed_rect();
            position->x = committed.x;
            position->y = committed.y;
            position->cx = committed.width;
            position->cy = committed.height;
            position->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
            return false;
        }
        case WM_KEYDOWN:
            if (sizing_active_ && event->wParam == VK_ESCAPE) {
                cancelled_ = true;
                transaction_.cancel();
                hide_outline();
            }
            return false;
        case WM_CANCELMODE:
            if (sizing_active_) {
                cancelled_ = true;
                transaction_.cancel();
                hide_outline();
            }
            return false;
        case WM_CAPTURECHANGED:
            // A normal mouse-button release can relinquish capture immediately
            // before WM_EXITSIZEMOVE.  Treat capture transfer, or capture loss
            // while the button is still down, as cancellation without turning
            // every successful drag into a cancelled transaction.
            if (sizing_active_
                && (event->lParam != 0 || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0)) {
                cancelled_ = true;
                transaction_.cancel();
                hide_outline();
            }
            return false;
        case WM_EXITSIZEMOVE: {
            interactive_loop_ = false;
            hide_outline();
            if (!sizing_active_) {
                return false;
            }
            sizing_active_ = false;
            const auto final_rect = transaction_.complete();
            if (!cancelled_ && final_rect.has_value()) {
                applying_final_rect_ = true;
                SetWindowPos(
                    event->hwnd,
                    nullptr,
                    final_rect->x,
                    final_rect->y,
                    final_rect->width,
                    final_rect->height,
                    SWP_NOACTIVATE | SWP_NOZORDER);
                applying_final_rect_ = false;
                schedule_viewport_commit(true);
            } else {
                set_active(false);
            }
            return false;
        }
        case WM_SYSCOMMAND: {
            const WPARAM command = event->wParam & 0xFFF0U;
            if (command == SC_MAXIMIZE || command == SC_MINIMIZE || command == SC_RESTORE) {
                set_active(true);
                state_change_pending_ = true;
            }
            return false;
        }
        case WM_DPICHANGED:
            set_active(true);
            state_change_pending_ = true;
            return false;
        case WM_SIZE:
            if (event->wParam == SIZE_MINIMIZED) {
                state_change_pending_ = false;
                set_active(false);
                return false;
            }
            if (state_change_pending_) {
                state_change_pending_ = false;
                schedule_viewport_commit(true);
            }
            return false;
        case WM_WINDOWPOSCHANGED:
            if (!interactive_loop_ && !applying_final_rect_ && !commit_queued_
                && event->lParam != 0) {
                const auto* position = reinterpret_cast<const WINDOWPOS*>(event->lParam);
                if ((position->flags & SWP_NOSIZE) == 0) {
                    schedule_viewport_commit(state_change_pending_);
                    state_change_pending_ = false;
                }
            }
            return false;
        default:
            return false;
        }
    }

    void show_outline(const RECT& rect) {
        if (outline_ == nullptr) {
            outline_ = std::make_unique<ResizeOutlineWidget>();
            outline_->winId();
        }
        SetWindowPos(
            reinterpret_cast<HWND>(outline_->winId()),
            HWND_TOPMOST,
            rect.left,
            rect.top,
            (std::max)(1L, rect.right - rect.left),
            (std::max)(1L, rect.bottom - rect.top),
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        outline_->update();
    }

    void hide_outline() {
        if (outline_ != nullptr) {
            outline_->hide();
        }
    }
#endif

    QPointer<QWidget> playback_window_;
    QPointer<QWidget> playback_canvas_;
    std::unique_ptr<ResizeOutlineWidget> outline_;
    PlaybackGeometryTransaction transaction_;
    ActiveChangedCallback active_callback_;
    ViewportCommittedCallback commit_callback_;
    PlaybackWindowGeometryController* filter_ = nullptr;
    QSize last_viewport_;
    std::uint64_t transaction_id_ = 0;
    std::uint64_t lifecycle_generation_ = 0;
    std::uint64_t geometry_preview_total_ = 0;
    std::uint64_t geometry_commit_total_ = 0;
    std::uint64_t viewport_commit_total_ = 0;
    bool started_ = false;
    bool transitions_disabled_ = false;
    bool transaction_active_ = false;
    bool interactive_loop_ = false;
    bool sizing_active_ = false;
    bool cancelled_ = false;
    bool applying_final_rect_ = false;
    bool state_change_pending_ = false;
    bool commit_queued_ = false;
};

PlaybackWindowGeometryController::PlaybackWindowGeometryController(
    QWidget* playback_window,
    QWidget* playback_canvas)
    : QObject(playback_window),
      impl_(std::make_unique<Impl>(playback_window, playback_canvas)) {}

PlaybackWindowGeometryController::~PlaybackWindowGeometryController() {
    stop();
}

void PlaybackWindowGeometryController::set_active_changed_callback(ActiveChangedCallback callback) {
    impl_->set_active_callback(std::move(callback));
}

void PlaybackWindowGeometryController::set_viewport_committed_callback(ViewportCommittedCallback callback) {
    impl_->set_commit_callback(std::move(callback));
}

void PlaybackWindowGeometryController::start() {
    impl_->start(this);
}

void PlaybackWindowGeometryController::stop() {
    impl_->stop();
}

void PlaybackWindowGeometryController::publish_initial_viewport() {
    impl_->publish_initial_viewport();
}

std::uint64_t PlaybackWindowGeometryController::geometry_preview_total() const noexcept {
    return impl_->geometry_preview_total_;
}

std::uint64_t PlaybackWindowGeometryController::geometry_commit_total() const noexcept {
    return impl_->geometry_commit_total_;
}

std::uint64_t PlaybackWindowGeometryController::viewport_commit_total() const noexcept {
    return impl_->viewport_commit_total_;
}

bool PlaybackWindowGeometryController::transitions_disabled() const noexcept {
    return impl_->transitions_disabled_;
}

bool PlaybackWindowGeometryController::nativeEventFilter(
    const QByteArray& event_type,
    void* message,
    qintptr* result) {
#if defined(_WIN32)
    if (event_type == "windows_generic_MSG" || event_type == "windows_dispatcher_MSG") {
        return impl_->handle_windows_message(static_cast<MSG*>(message), result);
    }
#else
    (void)event_type;
    (void)message;
    (void)result;
#endif
    return false;
}

}  // namespace redclaw::ui
