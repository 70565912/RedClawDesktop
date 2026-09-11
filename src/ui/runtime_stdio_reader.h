#pragma once
#include "runtime_control_queue.h"
#include "gui_latency_probe.h"
#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>
#include <functional>

namespace redclaw::ui {

class RuntimeStdioReader final : public QObject {
public:
    using Control = std::function<void(const redclaw::protocol::StreamControlMessageV1&, std::uint64_t)>;
    using Diagnostic = std::function<void(const QString&)>;
    using Failure = std::function<void(const QString&, bool)>;
    RuntimeStdioReader(QProcess& process, Control control, Diagnostic diagnostic, Failure failure, QObject* parent)
        : QObject(parent), process_(process), control_(std::move(control)), diagnostic_(std::move(diagnostic)), failure_(std::move(failure)) {
        control_timer_.setSingleShot(true); diagnostic_timer_.setSingleShot(true);
        control_timer_.setTimerType(Qt::PreciseTimer); diagnostic_timer_.setTimerType(Qt::PreciseTimer);
        QObject::connect(&control_timer_, &QTimer::timeout, this, [this] { drain_control(); });
        QObject::connect(&diagnostic_timer_, &QTimer::timeout, this, [this] { drain_diagnostic(); });
        QObject::connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { schedule(control_timer_); });
        QObject::connect(&process_, &QProcess::readyReadStandardError, this, [this] { schedule(diagnostic_timer_); });
        QObject::connect(&process_, &QProcess::started, this, [this] {
            control_queue_ = {}; diagnostic_buffer_.clear(); diagnostic_lost_ = false;
        });
        QObject::connect(&process_, &QProcess::finished, this, [this] {
            control_queue_.fail(); control_timer_.stop();
            failure_("Runtime control channel disconnected.", true);
        });
    }
    QJsonObject snapshot() const {
        return {{"control_bytes", qint64(control_queue_.bytes())}, {"control_messages", qint64(control_queue_.messages())},
            {"control_peak_bytes", qint64(control_queue_.peak_bytes())}, {"control_peak_messages", qint64(control_queue_.peak_messages())},
            {"control_failed", control_queue_.failed()}, {"diagnostic_pending_bytes", qint64(diagnostic_buffer_.size())},
            {"diagnostic_peak_bytes", qint64(diagnostic_peak_)}, {"diagnostic_lost", diagnostic_lost_}};
    }
private:
    static void schedule(QTimer& timer) { if (!timer.isActive()) timer.start(0); }
    qsizetype available(QProcess::ProcessChannel channel) { process_.setReadChannel(channel); return process_.bytesAvailable(); }
    void fail_control(const QString& error) {
        control_queue_.fail(); control_timer_.stop(); process_.closeReadChannel(QProcess::StandardOutput); failure_(error, true);
    }
    void record_queues() {
        if (auto* probe = gui_latency_probe()) {
            probe->queue(GuiLatencyProbe::Queue::kControlMessages, control_queue_.messages());
            probe->queue(GuiLatencyProbe::Queue::kControlBytes, control_queue_.bytes() + available(QProcess::StandardOutput));
            probe->queue(GuiLatencyProbe::Queue::kDiagnosticBytes, diagnostic_buffer_.size() + available(QProcess::StandardError));
        }
    }
    void drain_control() {
        GuiLatencyScope timing(GuiStage::kControlDrain);
        if (control_queue_.failed()) return;
        if (available(QProcess::StandardOutput) > RuntimeControlQueue::kByteCapacity - control_queue_.bytes()) {
            fail_control("Runtime control receive byte capacity exceeded."); return;
        }
        if (!control_queue_.append(process_.read(64 * 1024))) {
            fail_control("Runtime control receive frame/message capacity exceeded."); return;
        }
        record_queues();
        QElapsedTimer budget; budget.start();
        while (const auto frame = control_queue_.take()) {
            std::uint64_t received_us = 0;
            const auto parsed = redclaw::protocol::parse_local_runtime_control_frame_v2(frame->toStdString(), &received_us);
            if (!parsed.ok) { fail_control("Invalid dedicated runtime control frame."); return; }
            control_(parsed.value, received_us);
            if (budget.nsecsElapsed() >= 2000000) break;
        }
        if (control_queue_.ready() || available(QProcess::StandardOutput) > 0) schedule(control_timer_);
        record_queues();
    }
    void drain_diagnostic() {
        GuiLatencyScope timing(GuiStage::kStdoutDrain);
        if (diagnostic_lost_) return;
        constexpr qsizetype kDiagnosticCapacity = 4 * 1024 * 1024;
        if (available(QProcess::StandardError) > kDiagnosticCapacity - diagnostic_buffer_.size()) {
            diagnostic_lost_ = true; process_.closeReadChannel(QProcess::StandardError);
            failure_("Runtime diagnostics exceeded receive capacity; evidence is incomplete.", false); return;
        }
        diagnostic_buffer_.append(process_.read(64 * 1024));
        diagnostic_peak_ = std::max(diagnostic_peak_, diagnostic_buffer_.size());
        record_queues();
        QElapsedTimer budget; budget.start();
        for (int count = 0; count < 4; ++count) {
            const auto newline = diagnostic_buffer_.indexOf('\n');
            if (newline < 0) break;
            const auto line = diagnostic_buffer_.left(newline); diagnostic_buffer_.remove(0, newline + 1);
            const auto text = QString::fromLocal8Bit(line).trimmed();
            if (!text.isEmpty()) diagnostic_(text);
            if (budget.nsecsElapsed() >= 2000000) break;
        }
        if (diagnostic_buffer_.contains('\n') || available(QProcess::StandardError) > 0) schedule(diagnostic_timer_);
        record_queues();
    }
    QProcess& process_;
    Control control_;
    Diagnostic diagnostic_;
    Failure failure_;
    QTimer control_timer_, diagnostic_timer_;
    RuntimeControlQueue control_queue_;
    QByteArray diagnostic_buffer_;
    qsizetype diagnostic_peak_ = 0;
    bool diagnostic_lost_ = false;
};

}  // namespace redclaw::ui
