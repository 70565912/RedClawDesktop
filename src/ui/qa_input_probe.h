#pragma once
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "remote_input_capture.h"
#include "gui_latency_probe.h"
#include "gui_diagnostic_writer.h"
#include "redclaw/input/input_module.h"
#include <QAbstractNativeEventFilter>
#include <QJsonArray>
#include <QTimer>
#include <QWidget>
#include <Windows.h>
#include <cmath>
#include <array>
#include <deque>
#include <vector>

namespace redclaw::ui {

// Available only in explicitly launched local Debug fixtures. The native hook
// sees the marked message before Qt's remote-input loopback suppression.
class QaInputProbe final : public QObject, public QAbstractNativeEventFilter {
public:
    QaInputProbe(QWidget& canvas, ControllerRemoteInputCapture& capture)
        : canvas_(canvas), capture_(capture) {
        QCoreApplication::instance()->installNativeEventFilter(this);
        capture_.set_qa_observers(
            [this](const auto& message, auto time) { sent(message, time); },
            [this](const InputAckTiming& ack) { acknowledged(ack); });
        timer_.setTimerType(Qt::PreciseTimer); timer_.setInterval(25);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { step(); });
    }
    ~QaInputProbe() override {
        timer_.stop(); capture_.set_qa_observers({}, {});
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
    void set_geometry(const redclaw::protocol::StreamControlMessageV1& message) {
        const redclaw::input::DesktopGeometry geometry{message.desktop_origin_x, message.desktop_origin_y,
            message.desktop_width, message.desktop_height, message.desktop_rotation, message.desktop_geometry_revision};
        if (timer_.isActive() && geometry.revision != geometry_.revision) fail("capture_geometry_changed", true);
        geometry_ = geometry;
        if (!region_.width || region_.x + region_.width > geometry.width || region_.y + region_.height > geometry.height)
            region_ = {0, 0, geometry.width, geometry.height};
    }
    void set_region(const redclaw::protocol::StreamControlMessageV1& message) {
        if (timer_.isActive()) fail("capture_region_changed", true);
        const auto low = [](std::uint16_t v, std::uint32_t size) { return std::uint32_t(std::uint64_t(v) * size / 65535); };
        const auto high = [](std::uint16_t v, std::uint32_t size) { return std::uint32_t((std::uint64_t(v) * size + 65534) / 65535); };
        const auto x = low(message.region_left, geometry_.width), y = low(message.region_top, geometry_.height);
        region_ = {x, y, high(message.region_right, geometry_.width) - x, high(message.region_bottom, geometry_.height) - y};
        geometry_.revision = message.capture_region_revision;
    }
    static bool normalize_target(const redclaw::input::DesktopPoint& point,
        const redclaw::input::DesktopGeometry& geometry, const redclaw::input::DesktopCaptureRegion& region,
        std::uint16_t* x, std::uint16_t* y) {
        if (!x || !y || !redclaw::input::is_valid_desktop_geometry(geometry) || !region.width || !region.height) return false;
        double a = double(point.x - geometry.origin_x) / std::max(1U, geometry.width - 1);
        double b = double(point.y - geometry.origin_y) / std::max(1U, geometry.height - 1);
        double u = a, v = b;
        switch (geometry.rotation) {
        case 90: u = 1 - b; v = a; break;
        case 180: u = 1 - a; v = 1 - b; break;
        case 270: u = b; v = 1 - a; break;
        default: break;
        }
        u = (u * (geometry.width - 1) - region.x) / std::max(1U, region.width - 1);
        v = (v * (geometry.height - 1) - region.y) / std::max(1U, region.height - 1);
        if (u < 0 || v < 0 || u > 1 || v > 1) return false;
        *x = static_cast<std::uint16_t>(std::lround(u * 65535));
        *y = static_cast<std::uint16_t>(std::lround(v * 65535));
        return true;
    }
    bool start(QString* error) {
        if (!capture_.input_forwarding() || !capture_.keyboard_target_is_active()
            || geometry_.revision != capture_.desktop_geometry_revision()) {
            if (error) *error = "QA target focus, input lease and capture geometry must be ready"; return false;
        }
        target_ = reinterpret_cast<HWND>(canvas_.winId()); root_ = GetAncestor(target_, GA_ROOT);
        DWORD process = 0; GetWindowThreadProcessId(target_, &process);
        if (process != GetCurrentProcessId() || GetForegroundWindow() != root_ || !GetWindowRect(target_, &rectangle_)) {
            if (error) *error = "QA target window identity is not active"; return false;
        }
        const redclaw::input::DesktopPoint center{(rectangle_.left + rectangle_.right) / 2, (rectangle_.top + rectangle_.bottom) / 2};
        keyboard_target_ = GetFocus(); mouse_target_ = WindowFromPoint({center.x, center.y});
        DWORD keyboard_process = 0, mouse_process = 0;
        GetWindowThreadProcessId(keyboard_target_, &keyboard_process); GetWindowThreadProcessId(mouse_target_, &mouse_process);
        if (keyboard_process != process || mouse_process != process || GetAncestor(keyboard_target_, GA_ROOT) != root_
            || GetAncestor(mouse_target_, GA_ROOT) != root_) {
            if (error) *error = "QA keyboard focus and mouse hit target must belong to this playback window"; return false;
        }
        if (!normalize_target(center, geometry_, region_, &mouse_x_, &mouse_y_)) {
            if (error) *error = "QA target lies outside the actual capture region"; return false;
        }
        receipts_.clear(); receipts_.reserve(kCapacity);
        for (auto& pending : pending_) pending.clear();
        pending_count_ = 0; next_ack_ = 0;
        error_.clear(); interfered_ = false; overflow_ = 0; next_mouse_ = false; recording_ = true;
        started_us_ = gui_monotonic_us(); stopped_us_ = 0; dpi_ = GetDpiForWindow(target_);
        qt_focus_ = QApplication::focusWidget();
        timer_.start(); return true;
    }
    void stop() { if (timer_.isActive()) stopped_us_ = gui_monotonic_us(); timer_.stop(); }
    GuiDiagnosticWriter::Work export_work(const QString& path) const {
        return [path, metadata = snapshot(), samples = receipts_](QString* error) mutable {
            QJsonArray records;
            for (const auto& r : samples) records.append(QJsonObject{{"sequence", qint64(r.sequence)}, {"kind", int(r.kind)},
                {"sent_us", qint64(r.sent_us)}, {"received_us", qint64(r.received_us)},
                {"runtime_ack_us", qint64(r.runtime_ack_us)}, {"gui_ack_us", qint64(r.gui_ack_us)}});
            metadata.insert("receipts", records);
            return GuiDiagnosticWriter::write_status(path, metadata, error);
        };
    }
    QJsonObject snapshot() const {
        std::uint64_t keyboard = 0, mouse = 0, missing = 0;
        for (const auto& r : receipts_) {
            if (r.received_us) { if (r.kind < 2) ++keyboard; else ++mouse; } else ++missing;
        }
        QJsonObject result{{"schema", "redclaw.qa-native-input.v2"}, {"clock", "steady_microseconds"},
            {"ack_precision", "microseconds"}, {"receipt_scope", "actual_send_to_native_dispatch_key_and_left_button"},
            {"running", timer_.isActive()}, {"interfered", interfered_}, {"error", error_},
            {"start_us", qint64(started_us_)}, {"stop_us", qint64(stopped_us_)},
            {"target_hwnd", qint64(reinterpret_cast<std::uintptr_t>(target_))}, {"target_pid", qint64(GetCurrentProcessId())},
            {"keyboard_target_hwnd", qint64(reinterpret_cast<std::uintptr_t>(keyboard_target_))},
            {"mouse_target_hwnd", qint64(reinterpret_cast<std::uintptr_t>(mouse_target_))},
            {"dpi", int(dpi_)}, {"geometry_revision", qint64(geometry_.revision)},
            {"capture_width", int(geometry_.width)}, {"capture_height", int(geometry_.height)},
            {"capture_origin_x", geometry_.origin_x}, {"capture_origin_y", geometry_.origin_y}, {"capture_rotation", int(geometry_.rotation)},
            {"target_x", int(rectangle_.left)}, {"target_y", int(rectangle_.top)},
            {"target_width", int(rectangle_.right - rectangle_.left)}, {"target_height", int(rectangle_.bottom - rectangle_.top)},
            {"keyboard_received", qint64(keyboard)}, {"mouse_received", qint64(mouse)},
            {"missing_receipts", qint64(missing)}, {"samples", qint64(receipts_.size())},
            {"capacity", qint64(kCapacity)}, {"overflow", qint64(overflow_)},
            {"kinds", QJsonArray{"key_down", "key_up", "mouse_down", "mouse_up"}}};
        return result;
    }
    bool nativeEventFilter(const QByteArray&, void* message, qintptr*) override {
        if (!recording_ || !message) return false;
        const auto& msg = *static_cast<MSG*>(message);
        if (GetAncestor(msg.hwnd, GA_ROOT) != root_) return false;
        int kind = -1;
        switch (msg.message) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: kind = 0; break;
        case WM_KEYUP: case WM_SYSKEYUP: kind = 1; break;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: kind = 2; break;
        case WM_LBUTTONUP: kind = 3; break;
        default: return false;
        }
        if (static_cast<std::uintptr_t>(GetMessageExtraInfo()) != redclaw::input::kRedClawInputExtraInfo) {
            if (timer_.isActive()) fail("external_input", true);
            return false;
        }
        if (!pending_count_) return false;
        // Windows can deliver keyboard and mouse messages in different order
        // across types. Preserve sequence order within each native event kind.
        auto& pending = pending_[kind];
        if (pending.empty()) { fail("unexpected_native_event_kind", false); return false; }
        auto& receipt = receipts_[pending.front()];
        if (receipt.kind != kind || msg.hwnd != (kind < 2 ? keyboard_target_ : mouse_target_)) {
            fail(QString("unexpected_native_target_or_event expected_kind=%1 actual_kind=%2 hwnd=%3")
                .arg(receipt.kind).arg(kind).arg(quintptr(msg.hwnd)), false); return false;
        }
        receipt.received_us = gui_monotonic_us(); pending.pop_front(); --pending_count_;
        return false;
    }
private:
    struct Receipt { std::uint64_t sequence, sent_us, received_us = 0, runtime_ack_us = 0, gui_ack_us = 0; int kind; };
    static constexpr std::size_t kCapacity = 32768;
    void fail(const QString& error, bool interference) {
        if (error_.isEmpty()) error_ = error;
        interfered_ |= interference; stop();
    }
    void sent(const redclaw::protocol::StreamControlMessageV1& message, std::uint64_t time) {
        if (!sending_ || message.type != redclaw::protocol::StreamControlMessageTypeV1::kInputBatch) return;
        for (const auto& event : message.input_events) {
            using Type = redclaw::protocol::RemoteInputEventTypeV1;
            int kind = event.type == Type::kKeyDown ? 0 : event.type == Type::kKeyUp ? 1
                : event.type == Type::kMouseButtonDown ? 2 : event.type == Type::kMouseButtonUp ? 3 : -1;
            if (kind < 0) continue;
            if (receipts_.size() == kCapacity || pending_count_ == 256) { ++overflow_; fail("receipt_capacity_exceeded", false); return; }
            pending_[kind].push_back(receipts_.size()); ++pending_count_;
            receipts_.push_back({message.input_sequence, time, 0, 0, 0, kind});
        }
    }
    void acknowledged(const InputAckTiming& ack) {
        if (ack.kind != InputAckKind::kBatch) return;
        while (next_ack_ < receipts_.size() && receipts_[next_ack_].sequence <= ack.sequence) {
            auto& receipt = receipts_[next_ack_++];
            if (receipt.sequence == ack.sequence) { receipt.runtime_ack_us = ack.received_us; receipt.gui_ack_us = ack.consumed_us; }
        }
    }
    void step() {
        RECT current{};
        if (GetForegroundWindow() != root_ || GetFocus() != keyboard_target_ || QApplication::focusWidget() != qt_focus_ || !GetWindowRect(target_, &current)
            || !EqualRect(&current, &rectangle_) || GetDpiForWindow(target_) != dpi_) {
            fail("external_focus_or_geometry", true); return;
        }
        if (!capture_.input_forwarding() || !capture_.keyboard_target_is_active()
            || capture_.desktop_geometry_revision() != geometry_.revision) { fail("input_lease_or_geometry_unavailable", false); return; }
        QString error; sending_ = true;
        const bool sent_ok = next_mouse_
            ? capture_.submit_qa_mouse_click(mouse_x_, mouse_y_, redclaw::protocol::RemoteInputMouseButtonV1::kLeft, &error)
            : capture_.submit_qa_key_press(0x1e, 0x41, false, &error);
        sending_ = false; next_mouse_ = !next_mouse_;
        if (!sent_ok) fail(error, false);
    }
    QWidget& canvas_;
    QPointer<QWidget> qt_focus_;
    ControllerRemoteInputCapture& capture_;
    QTimer timer_;
    redclaw::input::DesktopGeometry geometry_;
    redclaw::input::DesktopCaptureRegion region_;
    HWND target_ = nullptr, root_ = nullptr, keyboard_target_ = nullptr, mouse_target_ = nullptr;
    RECT rectangle_{};
    UINT dpi_ = 0;
    std::uint16_t mouse_x_ = 0, mouse_y_ = 0;
    std::vector<Receipt> receipts_;
    std::array<std::deque<std::size_t>, 4> pending_;
    std::size_t pending_count_ = 0;
    std::size_t next_ack_ = 0;
    std::uint64_t started_us_ = 0, stopped_us_ = 0, overflow_ = 0;
    QString error_;
    bool recording_ = false, sending_ = false, next_mouse_ = false, interfered_ = false;
};

}  // namespace redclaw::ui
#endif
