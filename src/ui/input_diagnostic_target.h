#pragma once
#if defined(_WIN32)
#include "gui_diagnostic_writer.h"
#include "gui_latency_probe.h"
#include "redclaw/input/input_module.h"
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QJsonArray>
#include <QPainter>
#include <QTimer>
#include <QWidget>
#include <QWheelEvent>
#include <Windows.h>
#include <array>
#include <vector>

namespace redclaw::ui {
// Passive receiver, confined to its own ordinary desktop window. No global
// hook, input generation, key values, window text, or privilege changes.
class InputDiagnosticTarget final : public QWidget, public QAbstractNativeEventFilter {
public:
    InputDiagnosticTarget() {
        setWindowTitle("RedClaw input diagnostic target");
        resize(640, 360); setFocusPolicy(Qt::StrongFocus); setMouseTracking(true);
        records_.reserve(kCapacity);
        QCoreApplication::instance()->installNativeEventFilter(this);
        timer_.setInterval(250);
        connect(&timer_, &QTimer::timeout, this, [this] { update(); });
        timer_.start();
    }
    ~InputDiagnosticTarget() override {
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }
    void start(bool show_window = true) {
        marked_.fill(0); unmarked_.fill(0); qt_.fill(0); records_.clear(); overflow_ = 0;
        recording_ = true; started_us_ = gui_monotonic_us();
        (void)winId();
        if (show_window) { show(); raise(); activateWindow(); setFocus(); }
    }
    void stop() { recording_ = false; }
    QJsonObject snapshot() const {
        const auto counts = [](const auto& values) {
            QJsonArray result; for (const auto value : values) result.append(qint64(value)); return result;
        };
        return {{"schema", "redclaw.input-diagnostic-target.v1"}, {"recording", recording_},
            {"pid", qint64(GetCurrentProcessId())}, {"hwnd", qint64(internalWinId())},
            {"foreground", GetForegroundWindow() == reinterpret_cast<HWND>(internalWinId())},
            {"started_us", qint64(started_us_)}, {"sampled_us", qint64(gui_monotonic_us())},
            {"marked_native_counts", counts(marked_)}, {"unmarked_native_counts", counts(unmarked_)},
            {"qt_all_counts", counts(qt_)}, {"overflow", qint64(overflow_)},
            {"kinds", QJsonArray{"key_down", "key_up", "mouse_move", "button_down", "button_up", "wheel", "horizontal_wheel"}}};
    }
    GuiDiagnosticWriter::Work export_work(const QString& path) const {
        return [path, result = snapshot(), records = records_](QString* error) mutable {
            QJsonArray samples;
            for (const auto& r : records) samples.append(QJsonObject{
                {"at_us", qint64(r.at_us)}, {"kind", r.kind}, {"marked", r.marked}});
            result.insert("native_receipts", samples);
            return GuiDiagnosticWriter::write_status(path, result, error);
        };
    }
    bool nativeEventFilter(const QByteArray&, void* message, qintptr*) override {
        if (!recording_ || !message || !internalWinId()) return false;
        const auto& msg = *static_cast<MSG*>(message);
        if (msg.hwnd != reinterpret_cast<HWND>(internalWinId())) return false;
        int kind = -1;
        switch (msg.message) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: kind = 0; break;
        case WM_KEYUP: case WM_SYSKEYUP: kind = 1; break;
        case WM_MOUSEMOVE: kind = 2; break;
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK: kind = 3; break;
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: kind = 4; break;
        case WM_MOUSEWHEEL: kind = 5; break;
        case WM_MOUSEHWHEEL: kind = 6; break;
        default: return false;
        }
        const bool marked = static_cast<std::uintptr_t>(GetMessageExtraInfo()) == redclaw::input::kRedClawInputExtraInfo;
        ++(marked ? marked_ : unmarked_)[kind];
        if (records_.size() < kCapacity) records_.push_back({gui_monotonic_us(), kind, marked});
        else ++overflow_;
        return false;
    }
protected:
    bool event(QEvent* event) override {
        if (recording_) {
            int kind = -1;
            switch (event->type()) {
            case QEvent::KeyPress: kind = 0; break;
            case QEvent::KeyRelease: kind = 1; break;
            case QEvent::MouseMove: kind = 2; break;
            case QEvent::MouseButtonPress: case QEvent::MouseButtonDblClick: kind = 3; break;
            case QEvent::MouseButtonRelease: kind = 4; break;
            case QEvent::Wheel: {
                const auto delta = static_cast<QWheelEvent*>(event)->angleDelta();
                if (delta.y()) ++qt_[5];
                if (delta.x()) ++qt_[6];
                break;
            }
            default: break;
            }
            if (kind >= 0) ++qt_[kind];
        }
        return QWidget::event(event);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.fillRect(rect(), QColor(24, 34, 48)); p.setPen(Qt::white);
        const auto text = QString("INPUT DELIVERY TEST\n\nClick here, then press a normal key.\nNo key values or text are recorded.\n\n"
            "Windows marked messages: key %1 / %2   button %3 / %4   move %5\n"
            "Qt window events: key %6 / %7   button %8 / %9\n\nRecording: %10")
            .arg(marked_[0]).arg(marked_[1]).arg(marked_[3]).arg(marked_[4]).arg(marked_[2])
            .arg(qt_[0]).arg(qt_[1]).arg(qt_[3]).arg(qt_[4]).arg(recording_ ? "yes" : "no");
        p.drawText(rect().adjusted(20, 20, -20, -20), Qt::AlignLeft | Qt::TextWordWrap, text);
    }
private:
    struct Receipt { std::uint64_t at_us; int kind; bool marked; };
    static constexpr std::size_t kCapacity = 4096;
    QTimer timer_;
    std::array<std::uint64_t, 7> marked_{}, unmarked_{}, qt_{};
    std::vector<Receipt> records_;
    std::uint64_t started_us_ = 0, overflow_ = 0;
    bool recording_ = false;
};
}
#endif
