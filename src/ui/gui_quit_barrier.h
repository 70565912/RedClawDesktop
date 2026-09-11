#pragma once
#include "gui_diagnostic_writer.h"
#include <QCoreApplication>
#include <QEvent>
#include <QProcess>

namespace redclaw::ui {

// Keep the event dispatcher alive until runtime shutdown and final diagnostics
// finish. The worker completion, rather than a GUI wait/join, releases Quit.
class GuiQuitBarrier final : public QObject {
public:
    GuiQuitBarrier(QCoreApplication& app, QProcess& runtime, GuiDiagnosticWriter& writer,
        std::function<void()> request_stop, std::function<void()> prepare_final,
        GuiDiagnosticWriter::Work flush)
        : QObject(&app), app_(app), runtime_(runtime), writer_(writer),
          request_stop_(std::move(request_stop)), prepare_final_(std::move(prepare_final)), flush_(std::move(flush)) {
        app.installEventFilter(this);
        QObject::connect(&runtime, &QProcess::finished, this, [this] { finish_if_stopped(); });
    }
    ~GuiQuitBarrier() override { app_.removeEventFilter(this); }
protected:
    bool eventFilter(QObject* target, QEvent* event) override {
        if (target != &app_ || event->type() != QEvent::Quit || completed_) return false;
        if (!requested_) { requested_ = true; request_stop_(); finish_if_stopped(); }
        return true;
    }
private:
    void finish_if_stopped() {
        if (!requested_ || queued_ || runtime_.state() != QProcess::NotRunning) return;
        queued_ = true; prepare_final_();
        if (!writer_.after_pending(flush_, [this](bool ok, const QString&) {
            completed_ = true; app_.exit(ok ? 0 : 1);
        })) { completed_ = true; app_.exit(1); }
    }
    QCoreApplication& app_;
    QProcess& runtime_;
    GuiDiagnosticWriter& writer_;
    std::function<void()> request_stop_, prepare_final_;
    GuiDiagnosticWriter::Work flush_;
    bool requested_ = false, queued_ = false, completed_ = false;
};

}  // namespace redclaw::ui
