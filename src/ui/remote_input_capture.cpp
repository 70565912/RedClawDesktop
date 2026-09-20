#include "ui/remote_input_capture.h"
#include "ui/gui_latency_probe.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QDateTime>
#include <QEvent>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QStringList>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>

#include "redclaw/input/input_module.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace redclaw::ui {
namespace {

constexpr std::size_t kSendBatchEventLimit = 48;
constexpr std::size_t kApplicationAckSampleLimit = 128;

#if defined(_WIN32)
ControllerRemoteInputCapture* g_active_keyboard_capture = nullptr;

LRESULT CALLBACK remote_keyboard_hook(int code, WPARAM message, LPARAM data) {
    if (code >= 0 && g_active_keyboard_capture != nullptr) {
        const std::intptr_t handled = g_active_keyboard_capture->handle_low_level_keyboard(
            static_cast<std::uint32_t>(message), static_cast<std::uintptr_t>(data));
        if (handled != 0) {
            return static_cast<LRESULT>(handled);
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}
#endif

redclaw::protocol::RemoteInputMouseButtonV1 map_mouse_button(Qt::MouseButton button) {
    switch (button) {
    case Qt::LeftButton: return redclaw::protocol::RemoteInputMouseButtonV1::kLeft;
    case Qt::RightButton: return redclaw::protocol::RemoteInputMouseButtonV1::kRight;
    case Qt::MiddleButton: return redclaw::protocol::RemoteInputMouseButtonV1::kMiddle;
    case Qt::XButton1: return redclaw::protocol::RemoteInputMouseButtonV1::kX1;
    case Qt::XButton2: return redclaw::protocol::RemoteInputMouseButtonV1::kX2;
    default: return redclaw::protocol::RemoteInputMouseButtonV1::kNone;
    }
}

}  // namespace

ControllerRemoteInputCapture::ControllerRemoteInputCapture(QWidget* canvas, QObject* parent)
    : QObject(parent), canvas_(canvas), window_(canvas != nullptr ? canvas->window() : nullptr) {
    if (canvas_ != nullptr) {
        canvas_->setMouseTracking(true);
        canvas_->setFocusPolicy(Qt::StrongFocus);
        canvas_->installEventFilter(this);
    }
    if (window_ != nullptr && window_ != canvas_) {
        window_->installEventFilter(this);
    }
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->installEventFilter(this);
        QCoreApplication::instance()->installNativeEventFilter(this);
    }
    flush_timer_ = new QTimer(this);
    flush_timer_->setInterval(16);
    QObject::connect(flush_timer_, &QTimer::timeout, this, [this]() { flush_batch(); });
    state_sync_timer_ = new QTimer(this);
    state_sync_timer_->setInterval(500);
    QObject::connect(state_sync_timer_, &QTimer::timeout, this, [this]() { send_state_sync(); });
}

ControllerRemoteInputCapture::~ControllerRemoteInputCapture() {
    shutdown();
}

void ControllerRemoteInputCapture::shutdown() {
    // Window destruction emits focus/deactivation events after GUI-local
    // callbacks may have gone out of scope. Detach before tearing down widgets.
    paused_callback_ = {}; forwarding_changed_callback_ = {}; blocked_click_callback_ = {};
    qa_send_observer_ = {}; qa_ack_observer_ = {}; send_message_callback_ = {};
    if (QCoreApplication::instance() != nullptr) {
        QCoreApplication::instance()->removeEventFilter(this);
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
    if (canvas_) canvas_->removeEventFilter(this);
    if (window_ && window_ != canvas_) window_->removeEventFilter(this);
    pause(false, "capture shutdown"); canvas_ = nullptr; window_ = nullptr;
}

void ControllerRemoteInputCapture::set_send_message_callback(
    ControllerRemoteInputCapture::ControlSender callback) {
    send_message_callback_ = std::move(callback);
}

void ControllerRemoteInputCapture::set_paused_callback(
    ControllerRemoteInputCapture::PausedCallback callback) {
    paused_callback_ = std::move(callback);
}

void ControllerRemoteInputCapture::set_forwarding_changed_callback(
    ControllerRemoteInputCapture::ForwardingChangedCallback callback) {
    forwarding_changed_callback_ = std::move(callback);
}

void ControllerRemoteInputCapture::set_blocked_click_callback(
    ControllerRemoteInputCapture::BlockedClickCallback callback) {
    blocked_click_callback_ = std::move(callback);
}

void ControllerRemoteInputCapture::set_remote_frame_size(QSize size) {
    remote_frame_size_ = size.width() > 0 && size.height() > 0 ? size : QSize();
}

void ControllerRemoteInputCapture::set_qa_observers(QaSendObserver send, QaAckObserver ack) {
    qa_send_observer_ = std::move(send); qa_ack_observer_ = std::move(ack);
}

void ControllerRemoteInputCapture::set_desktop_geometry_revision(std::uint64_t revision) {
    desktop_geometry_revision_ = revision;
}

bool ControllerRemoteInputCapture::acknowledge_input_sequence(
    std::uint64_t sequence,
    std::uint64_t consumed_us, std::uint64_t runtime_received_us) {
    return pending_input_acks_.acknowledge(sequence, runtime_received_us, consumed_us, [this](const InputAckTiming& ack) {
        if (qa_ack_observer_) qa_ack_observer_(ack);
        if (auto* probe = gui_latency_probe()) probe->input_ack(ack);
        if (!ack.valid()) return;
        last_application_ack_rtt_ms_ = (ack.consumed_us - ack.sent_us) / 1000;
        application_ack_samples_ms_.push_back(last_application_ack_rtt_ms_);
        if (application_ack_samples_ms_.size() > kApplicationAckSampleLimit) {
            application_ack_samples_ms_.pop_front();
        }
    }) != 0;
}

bool ControllerRemoteInputCapture::activate(QString* error) {
    if (active_) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }
    if (canvas_ == nullptr || remote_frame_size_.isEmpty()) {
        if (error != nullptr) {
            *error = "A remote desktop frame is required before control can start.";
        }
        return false;
    }
    if (desktop_geometry_revision_ == 0) {
        if (error != nullptr) {
            *error = "A confirmed desktop geometry revision is required before control can start.";
        }
        return false;
    }
    if (!install_keyboard_capture(error)) {
        return false;
    }
    canvas_keyboard_target_ = true;
    canvas_->setFocus(Qt::OtherFocusReason);
    const std::uint32_t retained_flags = local_suspension_flags_
        & (static_cast<std::uint32_t>(LocalInputSuspensionReason::kGeometryTransaction)
            | static_cast<std::uint32_t>(LocalInputSuspensionReason::kWorkspaceTransfer));
    local_suspension_flags_ = retained_flags;
    if (window_ == nullptr || !window_->isVisible() || window_->isMinimized()) {
        local_suspension_flags_ |=
            static_cast<std::uint32_t>(LocalInputSuspensionReason::kHiddenOrMinimized);
    }
    if (!keyboard_target_is_active()) {
        local_suspension_flags_ |=
            static_cast<std::uint32_t>(LocalInputSuspensionReason::kWindowInactive);
    }
    active_ = true;
    pausing_ = false;
    suppressed_paste_key_ = false;
    local_suspension_release_sent_ = false;
    clear_local_input_state();
    pending_input_acks_.clear();
    flush_timer_->start();
    state_sync_timer_->start();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void ControllerRemoteInputCapture::pause(bool notify_peer, const QString& reason) {
    if (pausing_) {
        return;
    }
    pausing_ = true;
    const bool was_active = active_;
    active_ = false;
    canvas_keyboard_target_ = false;
    local_suspension_release_sent_ = false;
    local_suspension_flags_ = 0;
    flush_timer_->stop();
    state_sync_timer_->stop();
    uninstall_keyboard_capture();
    if (notify_peer && was_active) {
        send_release_all();
    }
    clear_local_input_state();
    pending_input_acks_.clear();
    pausing_ = false;
    if (was_active && paused_callback_) {
        paused_callback_(reason);
    }
}

void ControllerRemoteInputCapture::set_local_suspension(
    LocalInputSuspensionReason reason,
    bool suspended) {
    const std::uint32_t bit = static_cast<std::uint32_t>(reason);
    const std::uint32_t previous = local_suspension_flags_;
    if (suspended) {
        local_suspension_flags_ |= bit;
    } else {
        local_suspension_flags_ &= ~bit;
    }
    if (previous == local_suspension_flags_) {
        return;
    }
    if (active_ && suspended && !local_suspension_release_sent_) {
        clear_local_input_state();
        send_state_sync();
        local_suspension_release_sent_ = true;
    }
    if (local_suspension_flags_ == 0) {
        local_suspension_release_sent_ = false;
    }
    if (forwarding_changed_callback_) {
        forwarding_changed_callback_();
    }
}

bool ControllerRemoteInputCapture::submit_qa_mouse_click(
    std::uint16_t normalized_x,
    std::uint16_t normalized_y,
    redclaw::protocol::RemoteInputMouseButtonV1 button,
    QString* error) {
    if (!input_forwarding() || content_rect().isEmpty()) {
        if (error != nullptr) {
            *error = "Remote control must be active with a displayed frame.";
        }
        return false;
    }
    const std::uint32_t mask = button_mask(button);
    if (button == redclaw::protocol::RemoteInputMouseButtonV1::kNone || mask == 0) {
        if (error != nullptr) {
            *error = "The QA mouse button is invalid.";
        }
        return false;
    }
    if (critical_events_.size() + 2 > kCriticalQueueCapacity) {
        pause(true, "Remote input queue overflowed during QA mouse click.");
        if (error != nullptr) {
            *error = "Remote input queue overflowed.";
        }
        return false;
    }

    redclaw::protocol::RemoteInputEventV1 down;
    down.type = redclaw::protocol::RemoteInputEventTypeV1::kMouseButtonDown;
    down.normalized_x = normalized_x;
    down.normalized_y = normalized_y;
    down.mouse_button = button;
    redclaw::protocol::RemoteInputEventV1 up = down;
    up.type = redclaw::protocol::RemoteInputEventTypeV1::kMouseButtonUp;
    last_remote_pointer_position_ = std::pair{normalized_x, normalized_y};
    enqueue_critical(std::move(down));
    enqueue_critical(std::move(up));

    const std::uint64_t batches_before = sent_batch_count_;
    flush_batch();
    if (!active_ || sent_batch_count_ == batches_before) {
        if (error != nullptr) {
            *error = "The QA mouse click could not be sent.";
        }
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool ControllerRemoteInputCapture::submit_qa_key_press(
    std::uint16_t scan_code,
    std::uint16_t virtual_key,
    bool extended,
    QString* error) {
    if (!input_forwarding()) {
        if (error != nullptr) {
            *error = "Remote control must be active.";
        }
        return false;
    }
    if (!keyboard_target_is_active()) {
        if (error != nullptr) {
            *error = "The playback window must be the active foreground window.";
        }
        return false;
    }
    if (scan_code == 0 || scan_code > 0xFFU || virtual_key > 0xFFU) {
        if (error != nullptr) {
            *error = "The QA keyboard event is invalid.";
        }
        return false;
    }
    if (critical_events_.size() + 2 > kCriticalQueueCapacity) {
        pause(true, "Remote input queue overflowed during QA key press.");
        if (error != nullptr) {
            *error = "Remote input queue overflowed.";
        }
        return false;
    }

    redclaw::protocol::RemoteInputEventV1 down;
    down.type = redclaw::protocol::RemoteInputEventTypeV1::kKeyDown;
    down.scan_code = scan_code;
    down.virtual_key = virtual_key;
    down.extended = extended;
    redclaw::protocol::RemoteInputEventV1 up = down;
    up.type = redclaw::protocol::RemoteInputEventTypeV1::kKeyUp;
    enqueue_critical(std::move(down));
    enqueue_critical(std::move(up));

    const std::uint64_t batches_before = sent_batch_count_;
    flush_batch();
    if (!active_ || sent_batch_count_ == batches_before) {
        if (error != nullptr) {
            *error = "The QA keyboard event could not be sent.";
        }
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool ControllerRemoteInputCapture::active() const { return active_; }
bool ControllerRemoteInputCapture::control_enabled() const { return active_; }

bool ControllerRemoteInputCapture::input_forwarding() const {
    return active_ && local_suspension_flags_ == 0;
}

QString ControllerRemoteInputCapture::local_suspension_reason() const {
    QStringList reasons;
    if ((local_suspension_flags_
         & static_cast<std::uint32_t>(LocalInputSuspensionReason::kWindowInactive)) != 0) {
        reasons.push_back("window_inactive");
    }
    if ((local_suspension_flags_
         & static_cast<std::uint32_t>(LocalInputSuspensionReason::kHiddenOrMinimized)) != 0) {
        reasons.push_back("hidden_or_minimized");
    }
    if ((local_suspension_flags_
         & static_cast<std::uint32_t>(LocalInputSuspensionReason::kGeometryTransaction)) != 0) {
        reasons.push_back("geometry_transaction");
    }
    if ((local_suspension_flags_
         & static_cast<std::uint32_t>(LocalInputSuspensionReason::kLocalUiFocus)) != 0) {
        reasons.push_back("local_ui_focus");
    }
    if ((local_suspension_flags_
         & static_cast<std::uint32_t>(LocalInputSuspensionReason::kWorkspaceTransfer)) != 0) {
        reasons.push_back("workspace_transfer_busy");
    }
    return reasons.join(',');
}

QSize ControllerRemoteInputCapture::remote_frame_size() const { return remote_frame_size_; }

std::uint64_t ControllerRemoteInputCapture::desktop_geometry_revision() const {
    return desktop_geometry_revision_;
}

QRect ControllerRemoteInputCapture::content_rect() const {
    if (canvas_ == nullptr || remote_frame_size_.isEmpty()) {
        return {};
    }
    const QRect bounds = canvas_->contentsRect();
    if (bounds.isEmpty()) {
        return {};
    }
    const double scale = (std::min)(
        static_cast<double>(bounds.width()) / remote_frame_size_.width(),
        static_cast<double>(bounds.height()) / remote_frame_size_.height());
    const int width = (std::max)(1, static_cast<int>(std::lround(remote_frame_size_.width() * scale)));
    const int height = (std::max)(1, static_cast<int>(std::lround(remote_frame_size_.height() * scale)));
    return QRect(
        bounds.x() + (bounds.width() - width) / 2,
        bounds.y() + (bounds.height() - height) / 2,
        width,
        height);
}

bool ControllerRemoteInputCapture::map_content_position(
    const QPointF& position,
    std::uint16_t* x,
    std::uint16_t* y) const {
    return normalized_position(position, false, x, y);
}

bool ControllerRemoteInputCapture::keyboard_target_is_active() const {
    if (!canvas_keyboard_target_ || window_ == nullptr
        || !window_->isVisible() || window_->isMinimized()) {
        return false;
    }
#if defined(_WIN32)
    const auto hwnd = reinterpret_cast<HWND>(window_->internalWinId());
    return hwnd != nullptr && GetForegroundWindow() == hwnd;
#else
    return window_->isActiveWindow();
#endif
}
void ControllerRemoteInputCapture::set_clipboard_paste_callback(std::function<void(std::uint32_t)> callback) {
    clipboard_paste_callback_ = std::move(callback);
}
bool ControllerRemoteInputCapture::clipboard_paste_context_valid() const {
    return active_ && keyboard_target_is_active()
        && (local_suspension_flags_ & ~static_cast<std::uint32_t>(LocalInputSuspensionReason::kWorkspaceTransfer)) == 0;
}

bool ControllerRemoteInputCapture::should_suppress_mouse_loopback(
    QEvent::Type event_type,
    std::uintptr_t message_extra_info) {
    if (message_extra_info != redclaw::input::kRedClawInputExtraInfo) {
        return false;
    }
    switch (event_type) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonRelease:
    case QEvent::Wheel:
        return true;
    default:
        return false;
    }
}

std::size_t ControllerRemoteInputCapture::queued_critical_event_count() const {
    return critical_events_.size();
}

std::uint64_t ControllerRemoteInputCapture::sent_batch_count() const { return sent_batch_count_; }
std::uint64_t ControllerRemoteInputCapture::merged_mouse_move_count() const { return merged_mouse_move_count_; }
std::uint64_t ControllerRemoteInputCapture::suppressed_mouse_loopback_count() const {
    return suppressed_mouse_loopback_count_;
}
std::uint64_t ControllerRemoteInputCapture::blocked_control_click_hint_count() const {
    return blocked_control_click_hint_count_;
}
std::uint64_t ControllerRemoteInputCapture::last_application_ack_rtt_ms() const {
    return last_application_ack_rtt_ms_;
}

std::uint64_t ControllerRemoteInputCapture::application_ack_p95_ms() const {
    if (application_ack_samples_ms_.empty()) {
        return 0;
    }
    std::vector<std::uint64_t> sorted(
        application_ack_samples_ms_.begin(), application_ack_samples_ms_.end());
    std::sort(sorted.begin(), sorted.end());
    const std::size_t index = (sorted.size() * 95U + 99U) / 100U - 1U;
    return sorted[index];
}

bool ControllerRemoteInputCapture::eventFilter(QObject* watched, QEvent* event) {
    if (event == nullptr) {
        return QObject::eventFilter(watched, event);
    }
    auto* event_widget = qobject_cast<QWidget*>(watched);
    if (event_widget != nullptr && is_playback_ui_widget(event_widget)) {
        if (event->type() == QEvent::MouseButtonPress) {
            const bool canvas_target = is_canvas_widget(event_widget);
            if (canvas_target && canvas_ != nullptr) {
                canvas_->setFocus(Qt::MouseFocusReason);
            }
            update_keyboard_target(canvas_target);
        } else if (event->type() == QEvent::FocusIn && event_widget != window_) {
            // Owned tools may restore focus to the canvas when hidden. Only a
            // canvas mouse press may resume after local UI interaction.
            if (!is_canvas_widget(event_widget)) update_keyboard_target(false);
        }
    }
    if (watched == window_ && watched != canvas_) {
        if (active_ && event->type() == QEvent::Close) {
            pause(true, "Playback window was closed.");
        } else if (event->type() == QEvent::WindowDeactivate) {
            set_local_suspension(LocalInputSuspensionReason::kWindowInactive, true);
        } else if (event->type() == QEvent::WindowActivate) {
            set_local_suspension(LocalInputSuspensionReason::kWindowInactive, false);
            set_local_suspension(
                LocalInputSuspensionReason::kHiddenOrMinimized,
                window_ == nullptr || !window_->isVisible() || window_->isMinimized());
        } else if (event->type() == QEvent::Hide) {
            set_local_suspension(LocalInputSuspensionReason::kHiddenOrMinimized, true);
        } else if (event->type() == QEvent::Show
                   || event->type() == QEvent::WindowStateChange) {
            set_local_suspension(
                LocalInputSuspensionReason::kHiddenOrMinimized,
                window_ == nullptr || !window_->isVisible() || window_->isMinimized());
        }
        return QObject::eventFilter(watched, event);
    }
    if (watched != canvas_) {
        return QObject::eventFilter(watched, event);
    }
    if (!active_) {
        if (event->type() == QEvent::MouseButtonPress) {
            ++blocked_control_click_hint_count_;
            if (blocked_click_callback_) {
                blocked_click_callback_();
            }
            return true;
        }
        return QObject::eventFilter(watched, event);
    }
    if (!input_forwarding()) {
        if (event->type() == QEvent::MouseButtonPress
            && (local_suspension_flags_
                & static_cast<std::uint32_t>(LocalInputSuspensionReason::kLocalUiFocus)) != 0) {
            update_keyboard_target(true);
        }
        if (!input_forwarding()) {
            switch (event->type()) {
            case QEvent::MouseMove:
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonDblClick:
            case QEvent::MouseButtonRelease:
            case QEvent::Wheel:
                return true;
            default:
                return QObject::eventFilter(watched, event);
            }
        }
    }
    if (event->type() == QEvent::MouseMove) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        std::uint16_t x = 0;
        std::uint16_t y = 0;
        if (map_content_position(mouse->position(), &x, &y)) {
            last_remote_pointer_position_ = std::pair{x, y};
            enqueue_mouse_move(x, y);
        }
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress
        || event->type() == QEvent::MouseButtonDblClick
        || event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        std::uint16_t x = 0;
        std::uint16_t y = 0;
        const auto button = map_mouse_button(mouse->button());
        const std::uint32_t mask = button_mask(button);
        bool has_position = map_content_position(mouse->position(), &x, &y);
        const bool releasing_tracked_button = event->type() == QEvent::MouseButtonRelease
            && mask != 0 && (pressed_mouse_buttons_ & mask) != 0;
        if (!has_position && releasing_tracked_button && last_remote_pointer_position_.has_value()) {
            x = last_remote_pointer_position_->first;
            y = last_remote_pointer_position_->second;
            has_position = true;
        }
        if (button != redclaw::protocol::RemoteInputMouseButtonV1::kNone && has_position
            && (event->type() != QEvent::MouseButtonRelease || releasing_tracked_button)) {
            redclaw::protocol::RemoteInputEventV1 input;
            input.type = event->type() != QEvent::MouseButtonRelease
                ? redclaw::protocol::RemoteInputEventTypeV1::kMouseButtonDown
                : redclaw::protocol::RemoteInputEventTypeV1::kMouseButtonUp;
            input.normalized_x = x;
            input.normalized_y = y;
            input.mouse_button = button;
            if (event->type() != QEvent::MouseButtonRelease) {
                pressed_mouse_buttons_ |= mask;
                last_remote_pointer_position_ = std::pair{x, y};
            } else {
                pressed_mouse_buttons_ &= ~mask;
            }
            enqueue_critical(std::move(input));
        }
        return true;
    }
    if (event->type() == QEvent::Wheel) {
        auto* wheel = static_cast<QWheelEvent*>(event);
        std::uint16_t x = 0;
        std::uint16_t y = 0;
        if (map_content_position(wheel->position(), &x, &y)) {
            last_remote_pointer_position_ = std::pair{x, y};
            const QPoint delta = wheel->angleDelta();
            if (delta.y() != 0) {
                redclaw::protocol::RemoteInputEventV1 input;
                input.type = redclaw::protocol::RemoteInputEventTypeV1::kMouseWheel;
                input.normalized_x = x;
                input.normalized_y = y;
                input.wheel_delta = delta.y();
                enqueue_critical(std::move(input));
            }
            if (delta.x() != 0) {
                redclaw::protocol::RemoteInputEventV1 input;
                input.type = redclaw::protocol::RemoteInputEventTypeV1::kMouseHorizontalWheel;
                input.normalized_x = x;
                input.normalized_y = y;
                input.wheel_delta = delta.x();
                enqueue_critical(std::move(input));
            }
        }
        return true;
    }
    return QObject::eventFilter(watched, event);
}

bool ControllerRemoteInputCapture::nativeEventFilter(const QByteArray&, void* message, qintptr* result) {
#if defined(_WIN32)
    // Qt may queue/coalesce mouse events after the current Windows message has
    // changed. Its later QWidget event cannot recover GetMessageExtraInfo().
    if (!message || !canvas_) return false;
    const auto& msg = *static_cast<MSG*>(message);
    const auto target = reinterpret_cast<HWND>(canvas_->internalWinId());
    if (!target || (msg.hwnd != target && !IsChild(target, msg.hwnd))) return false;
    QEvent::Type type = QEvent::None;
    switch (msg.message) {
    case WM_MOUSEMOVE: type = QEvent::MouseMove; break;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        type = QEvent::MouseButtonPress; break;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP:
        type = QEvent::MouseButtonRelease; break;
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
        type = QEvent::MouseButtonDblClick; break;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: type = QEvent::Wheel; break;
    default: return false;
    }
    if (should_suppress_mouse_loopback(type, static_cast<std::uintptr_t>(GetMessageExtraInfo()))) {
        ++suppressed_mouse_loopback_count_;
        if (result) *result = 0;
        return true;
    }
#else
    (void)message; (void)result;
#endif
    return false;
}

bool ControllerRemoteInputCapture::install_keyboard_capture(QString* error) {
#if !defined(_WIN32)
    if (error != nullptr) {
        *error = "Low-level keyboard capture is only supported on Windows.";
    }
    return false;
#else
    if (keyboard_hook_ != nullptr) {
        return true;
    }
    if (g_active_keyboard_capture != nullptr && g_active_keyboard_capture != this) {
        if (error != nullptr) {
            *error = "Another RedClaw keyboard capture is already active.";
        }
        return false;
    }
    HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, remote_keyboard_hook, GetModuleHandleW(nullptr), 0);
    if (hook == nullptr) {
        if (error != nullptr) {
            *error = QString("Failed to install the keyboard capture hook (error %1).").arg(GetLastError());
        }
        return false;
    }
    keyboard_hook_ = hook;
    g_active_keyboard_capture = this;
    return true;
#endif
}

void ControllerRemoteInputCapture::uninstall_keyboard_capture() {
#if defined(_WIN32)
    if (keyboard_hook_ != nullptr) {
        UnhookWindowsHookEx(static_cast<HHOOK>(keyboard_hook_));
        keyboard_hook_ = nullptr;
    }
    if (g_active_keyboard_capture == this) {
        g_active_keyboard_capture = nullptr;
    }
#endif
}

void ControllerRemoteInputCapture::enqueue_critical(
    redclaw::protocol::RemoteInputEventV1 event) {
    if (!input_forwarding()) {
        return;
    }
    if (latest_mouse_move_.has_value()) {
        if (critical_events_.size() >= kCriticalQueueCapacity) {
            pause(true, "Remote input queue overflowed.");
            return;
        }
        critical_events_.push_back(std::move(*latest_mouse_move_));
        latest_mouse_move_.reset();
    }
    if (critical_events_.size() >= kCriticalQueueCapacity) {
        pause(true, "Remote input queue overflowed.");
        return;
    }
    critical_events_.push_back(std::move(event));
}

void ControllerRemoteInputCapture::enqueue_mouse_move(
    std::uint16_t normalized_x,
    std::uint16_t normalized_y) {
    if (!input_forwarding()) {
        return;
    }
    redclaw::protocol::RemoteInputEventV1 event;
    event.type = redclaw::protocol::RemoteInputEventTypeV1::kMouseMove;
    event.normalized_x = normalized_x;
    event.normalized_y = normalized_y;
    if (latest_mouse_move_.has_value()) {
        ++merged_mouse_move_count_;
    }
    latest_mouse_move_ = event;
}

void ControllerRemoteInputCapture::flush_batch() {
    if (!input_forwarding()) {
        return;
    }
    redclaw::protocol::StreamControlMessageV1 message;
    message.type = redclaw::protocol::StreamControlMessageTypeV1::kInputBatch;
    message.input_sequence = ++input_sequence_;
    message.input_events.reserve(kSendBatchEventLimit);
    while (!critical_events_.empty()
           && message.input_events.size() < kSendBatchEventLimit) {
        message.input_events.push_back(std::move(critical_events_.front()));
        critical_events_.pop_front();
    }
    if (message.input_events.size() < kSendBatchEventLimit
        && latest_mouse_move_.has_value()) {
        message.input_events.push_back(std::move(*latest_mouse_move_));
        latest_mouse_move_.reset();
    }
    if (message.input_events.empty()) {
        --input_sequence_;
        return;
    }
    if (!send_message(std::move(message))) {
        pause(true, "The runtime input channel is unavailable.");
        return;
    }
    ++sent_batch_count_;
}

void ControllerRemoteInputCapture::send_state_sync() {
    if (!active_) {
        return;
    }
    redclaw::protocol::StreamControlMessageV1 message;
    message.type = redclaw::protocol::StreamControlMessageTypeV1::kInputStateSync;
    message.input_sequence = ++input_sequence_;
    message.pressed_mouse_buttons = pressed_mouse_buttons_;
    message.pressed_scan_codes.reserve(pressed_keys_.size());
    for (const std::uint32_t key : pressed_keys_) {
        const std::uint16_t encoded = static_cast<std::uint16_t>(key & 0x7FFFU)
            | ((key & 0x10000U) != 0 ? 0x8000U : 0U);
        message.pressed_scan_codes.push_back(encoded);
    }
    if (!send_message(std::move(message))) {
        pause(true, "The runtime input channel is unavailable.");
        return;
    }
}

void ControllerRemoteInputCapture::send_release_all() {
    redclaw::protocol::StreamControlMessageV1 message;
    message.type = redclaw::protocol::StreamControlMessageTypeV1::kInputReleaseAll;
    (void)send_message(std::move(message));
}

void ControllerRemoteInputCapture::clear_local_input_state() {
    critical_events_.clear();
    latest_mouse_move_.reset();
    pressed_keys_.clear();
    pressed_virtual_keys_.clear();
    pressed_mouse_buttons_ = 0;
    last_remote_pointer_position_.reset();
}

bool ControllerRemoteInputCapture::send_message(
    redclaw::protocol::StreamControlMessageV1 message) {
    if (!send_message_callback_) {
        return false;
    }
    message.session_epoch = "local";
    message.message_id = ++local_message_id_;
    message.sent_at_ms = static_cast<std::uint64_t>(QDateTime::currentMSecsSinceEpoch());
    if (message.type == redclaw::protocol::StreamControlMessageTypeV1::kInputBatch
        || message.type == redclaw::protocol::StreamControlMessageTypeV1::kInputStateSync) {
        message.desktop_geometry_revision = desktop_geometry_revision_;
    }
    QString error;
    const auto sent_us = gui_monotonic_us();
    if (message.type == redclaw::protocol::StreamControlMessageTypeV1::kInputBatch
        || message.type == redclaw::protocol::StreamControlMessageTypeV1::kInputStateSync) {
        const auto kind = message.type == redclaw::protocol::StreamControlMessageTypeV1::kInputBatch ? InputAckKind::kBatch
            : message.pressed_scan_codes.empty() && message.pressed_mouse_buttons == 0 ? InputAckKind::kEmptySync : InputAckKind::kStateSync;
        if (!pending_input_acks_.remember(message.input_sequence, sent_us, kind)) return false;
    }
    if (qa_send_observer_) qa_send_observer_(message, sent_us);
    const bool sent = send_message_callback_(message, &error);
    if (sent && message.type == redclaw::protocol::StreamControlMessageTypeV1::kInputBatch) {
        for (const auto& event : message.input_events) {
            const auto kind = static_cast<std::size_t>(event.type);
            if (kind < sent_event_counts_.size()) ++sent_event_counts_[kind];
        }
    }
    return sent;
}

bool ControllerRemoteInputCapture::normalized_position(
    const QPointF& position,
    bool clamp_to_content,
    std::uint16_t* x,
    std::uint16_t* y) const {
    const QRect content = content_rect();
    if (content.isEmpty() || x == nullptr || y == nullptr) {
        return false;
    }
    QPointF mapped = position;
    if (!content.contains(mapped.toPoint())) {
        if (!clamp_to_content) {
            return false;
        }
        mapped.setX(std::clamp(mapped.x(), static_cast<double>(content.left()),
                               static_cast<double>(content.right())));
        mapped.setY(std::clamp(mapped.y(), static_cast<double>(content.top()),
                               static_cast<double>(content.bottom())));
    }
    const auto normalize = [](double value, int origin, int extent) {
        if (extent <= 1) {
            return static_cast<std::uint16_t>(0);
        }
        const double scaled = (value - origin) * 65535.0 / static_cast<double>(extent - 1);
        return static_cast<std::uint16_t>(std::clamp(std::lround(scaled), 0L, 65535L));
    };
    *x = normalize(mapped.x(), content.left(), content.width());
    *y = normalize(mapped.y(), content.top(), content.height());
    return true;
}

bool ControllerRemoteInputCapture::is_canvas_widget(const QWidget* widget) const {
    return widget != nullptr && canvas_ != nullptr
        && (widget == canvas_ || canvas_->isAncestorOf(widget));
}

bool ControllerRemoteInputCapture::is_playback_ui_widget(const QWidget* widget) const {
    for (const QWidget* current = widget; current != nullptr; current = current->parentWidget()) {
        if (current == window_) {
            return true;
        }
    }
    return false;
}

void ControllerRemoteInputCapture::update_keyboard_target(bool canvas_target) {
    canvas_keyboard_target_ = canvas_target;
    set_local_suspension(LocalInputSuspensionReason::kLocalUiFocus, !canvas_target);
}

std::uint32_t ControllerRemoteInputCapture::button_mask(
    redclaw::protocol::RemoteInputMouseButtonV1 button) {
    switch (button) {
    case redclaw::protocol::RemoteInputMouseButtonV1::kLeft: return 1U << 0U;
    case redclaw::protocol::RemoteInputMouseButtonV1::kRight: return 1U << 1U;
    case redclaw::protocol::RemoteInputMouseButtonV1::kMiddle: return 1U << 2U;
    case redclaw::protocol::RemoteInputMouseButtonV1::kX1: return 1U << 3U;
    case redclaw::protocol::RemoteInputMouseButtonV1::kX2: return 1U << 4U;
    case redclaw::protocol::RemoteInputMouseButtonV1::kNone: return 0;
    }
    return 0;
}

#if defined(_WIN32)
std::intptr_t ControllerRemoteInputCapture::handle_low_level_keyboard(
    std::uint32_t message,
    std::uintptr_t event_data) {
    if (!active_ || event_data == 0) {
        return 0;
    }
    const auto* keyboard = reinterpret_cast<const KBDLLHOOKSTRUCT*>(event_data);
    if ((keyboard->flags & LLKHF_INJECTED) != 0) {
        return 0;
    }
    const bool key_down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
    const bool key_up = message == WM_KEYUP || message == WM_SYSKEYUP;
    const std::uint16_t virtual_key = static_cast<std::uint16_t>(keyboard->vkCode);
    if (virtual_key == 'V' && suppressed_paste_key_ && (key_down || key_up)) {
        if (key_up) suppressed_paste_key_ = false;
        return 1; // includes repeats while the transfer temporarily suspends forwarding
    }
    if (!input_forwarding() || !keyboard_target_is_active()) {
        return 0;
    }
    if (!key_down && !key_up) {
        return 0;
    }
    if (key_down) {
        pressed_virtual_keys_.insert(virtual_key);
    } else {
        pressed_virtual_keys_.erase(virtual_key);
    }
    const auto has_any = [&](std::initializer_list<std::uint16_t> keys) {
        return std::any_of(keys.begin(), keys.end(), [&](std::uint16_t key) {
            return pressed_virtual_keys_.contains(key) || (GetAsyncKeyState(key) & 0x8000) != 0;
        });
    };
    const bool emergency = key_down && virtual_key == VK_ESCAPE
        && has_any({VK_CONTROL, VK_LCONTROL, VK_RCONTROL})
        && has_any({VK_MENU, VK_LMENU, VK_RMENU})
        && has_any({VK_SHIFT, VK_LSHIFT, VK_RSHIFT});
    if (key_down && virtual_key == 'V' && clipboard_paste_callback_
        && has_any({VK_CONTROL, VK_LCONTROL, VK_RCONTROL})
        && !has_any({VK_MENU, VK_LMENU, VK_RMENU, VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN})) {
        suppressed_paste_key_ = true;
        const auto sequence = GetClipboardSequenceNumber();
        // Empty state sync releases held keys while preserving the explicit
        // control grant. InputReleaseAll intentionally ends control on Host.
        clear_local_input_state(); send_state_sync();
        if (!active_) return 1;
        clipboard_paste_callback_(sequence);
        return 1;
    }
    if (emergency) {
        pause(true, "Emergency shortcut Ctrl+Alt+Shift+Esc was pressed.");
        return 1;
    }
    if (virtual_key == VK_DELETE
        && has_any({VK_CONTROL, VK_LCONTROL, VK_RCONTROL})
        && has_any({VK_MENU, VK_LMENU, VK_RMENU})) {
        return 1;
    }
    const bool extended = (keyboard->flags & LLKHF_EXTENDED) != 0;
    const std::uint32_t identity = keyboard->scanCode | (extended ? 0x10000U : 0U);
    redclaw::protocol::RemoteInputEventV1 event;
    event.type = key_down
        ? redclaw::protocol::RemoteInputEventTypeV1::kKeyDown
        : redclaw::protocol::RemoteInputEventTypeV1::kKeyUp;
    event.scan_code = static_cast<std::uint16_t>(keyboard->scanCode);
    event.virtual_key = virtual_key;
    event.extended = extended;
    event.repeat = key_down && pressed_keys_.contains(identity);
    if (key_down) {
        pressed_keys_.insert(identity);
    } else {
        pressed_keys_.erase(identity);
    }
    enqueue_critical(std::move(event));
    return 1;
}
#endif

}  // namespace redclaw::ui
