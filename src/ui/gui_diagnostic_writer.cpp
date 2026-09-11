#include "gui_diagnostic_writer.h"
#include "gui_latency_probe.h"

#include <algorithm>
#include <exception>
#include <QJsonDocument>
#include <QMetaObject>
#include <QSaveFile>

namespace redclaw::ui {

GuiDiagnosticWriter::GuiDiagnosticWriter(LogSink log_sink, StatusSink status_sink, QObject* parent)
    : QObject(parent), log_sink_(std::move(log_sink)),
      status_sink_(status_sink ? std::move(status_sink) : write_status) {
    worker_ = std::thread([this] { run(); });
}

GuiDiagnosticWriter::~GuiDiagnosticWriter() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
}

bool GuiDiagnosticWriter::post_log(LogRecord record) {
    const auto bytes = sizeof(QueuedLog) + static_cast<std::size_t>(record.text.size()
        + (record.source.size() + record.append_path.size()) * 2);
    std::size_t queued_bytes = 0;
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || bytes > kLogCapacity - log_bytes_) {
            ++dropped_logs_; dropped_bytes_ += bytes; return false;
        }
        log_bytes_ += bytes; log_peak_ = std::max(log_peak_, log_bytes_);
        logs_.push_back({++accepted_logs_, bytes, std::move(record)});
        queued_bytes = log_bytes_;
    }
    if (auto* probe = gui_latency_probe()) probe->queue(GuiLatencyProbe::Queue::kDiagnosticWriteBytes, queued_bytes);
    wake_.notify_one(); return true;
}

std::uint64_t GuiDiagnosticWriter::submit_status(QString path, StatusFactory status) {
    std::uint64_t version;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return 0;
        version = ++submitted_version_;
        if (pending_status_) ++coalesced_status_;
        pending_status_ = Status{version, std::move(path), std::move(status)};
    }
    wake_.notify_one(); return version;
}

bool GuiDiagnosticWriter::after_pending(Work work, Completion completion) {
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || barriers_.size() >= 8) return false;
        barriers_.push_back({accepted_logs_, submitted_version_, std::move(work), std::move(completion)});
    }
    wake_.notify_one(); return true;
}

QJsonObject GuiDiagnosticWriter::snapshot() const {
    std::lock_guard lock(mutex_);
    if (auto* probe = gui_latency_probe()) probe->queue(GuiLatencyProbe::Queue::kDiagnosticWriteBytes, log_bytes_);
    return {{"log_capacity_bytes", qint64(kLogCapacity)}, {"log_queued_bytes", qint64(log_bytes_)},
        {"log_queue_peak_bytes", qint64(log_peak_)}, {"log_submitted", qint64(accepted_logs_)},
        {"log_completed", qint64(completed_logs_)}, {"log_dropped", qint64(dropped_logs_)},
        {"log_dropped_bytes", qint64(dropped_bytes_)}, {"status_submitted_version", qint64(submitted_version_)},
        {"status_persisted_version", qint64(persisted_version_)}, {"status_writing_version", qint64(writing_version_)},
        {"status_pending_version", qint64(pending_status_ ? pending_status_->version : 0)},
        {"status_coalesced", qint64(coalesced_status_)}, {"failures", qint64(failures_)},
        {"last_error", last_error_}, {"evidence_integrity", dropped_logs_ == 0 && failures_ == 0}};
}

void GuiDiagnosticWriter::failure_locked(const QString& error) {
    ++failures_; last_error_ = error.isEmpty() ? QStringLiteral("diagnostic write failed") : error.left(1024);
}

void GuiDiagnosticWriter::record_external_loss(const QString& error) {
    std::lock_guard lock(mutex_); failure_locked(error);
}

bool GuiDiagnosticWriter::write_status(const QString& path, const QJsonObject& status, QString* error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { if (error) *error = file.errorString(); return false; }
    const auto bytes = QJsonDocument(status).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error) *error = file.errorString(); return false;
    }
    return true;
}

void GuiDiagnosticWriter::run() {
    bool prefer_log = false;
    for (;;) {
        std::optional<QueuedLog> log;
        std::optional<Status> status;
        std::optional<Barrier> barrier;
        bool integrity = true;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !logs_.empty() || pending_status_ || !barriers_.empty(); });
            if (!barriers_.empty() && completed_logs_ >= barriers_.front().log_sequence
                && completed_version_ >= barriers_.front().status_version) {
                barrier = std::move(barriers_.front()); barriers_.pop_front();
                integrity = failures_ == 0 && dropped_logs_ == 0;
            } else if (pending_status_ && (!prefer_log || logs_.empty())) {
                status = std::move(pending_status_); pending_status_.reset(); writing_version_ = status->version;
                prefer_log = true;
            } else if (!logs_.empty()) {
                log = std::move(logs_.front()); logs_.pop_front();
                prefer_log = false;
            } else if (stopping_) break;
        }
        QString error;
        bool ok = true;
        try {
            if (log) ok = log_sink_ && log_sink_(log->record, &error);
            if (status) {
                auto object = status->make();
                object.insert("diagnostic_status_version", qint64(status->version));
                ok = status_sink_(status->path, object, &error);
            }
            if (barrier && barrier->work) ok = barrier->work(&error);
        } catch (const std::exception& exception) {
            ok = false; error = QString::fromUtf8(exception.what());
        } catch (...) { ok = false; error = "diagnostic worker exception"; }
        {
            std::lock_guard lock(mutex_);
            if (!ok) failure_locked(error);
            if (log) { completed_logs_ = log->sequence; log_bytes_ -= log->bytes; }
            if (status) {
                completed_version_ = status->version; writing_version_ = 0;
                if (ok) persisted_version_ = status->version;
            }
            if (barrier && (!integrity || !ok) && error.isEmpty())
                error = last_error_.isEmpty() ? "diagnostic evidence is incomplete" : last_error_;
        }
        if (barrier && barrier->completion) {
            QMetaObject::invokeMethod(this,
                [completion = std::move(barrier->completion), ok = ok && integrity, error] { completion(ok, error); },
                Qt::QueuedConnection);
        }
    }
}

}  // namespace redclaw::ui
