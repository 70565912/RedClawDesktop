#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

namespace redclaw::ui {

// The GUI owns this worker. Payloads are immutable after submission; no sink
// or completion executes while the queue mutex is held.
class GuiDiagnosticWriter final : public QObject {
public:
    struct LogRecord { QString source; QByteArray text; bool flush = false; QString append_path; };
    using LogSink = std::function<bool(const LogRecord&, QString*)>;
    using StatusSink = std::function<bool(const QString&, const QJsonObject&, QString*)>;
    using StatusFactory = std::function<QJsonObject()>;
    using Work = std::function<bool(QString*)>;
    using Completion = std::function<void(bool, const QString&)>;
    static constexpr std::size_t kLogCapacity = 4U * 1024U * 1024U;

    explicit GuiDiagnosticWriter(LogSink log_sink, StatusSink status_sink = {}, QObject* parent = nullptr);
    ~GuiDiagnosticWriter() override;
    bool post_log(LogRecord record);
    std::uint64_t submit_status(QString path, StatusFactory status);
    // A job runs after all accepted logs and the status version preceding it.
    // Completion is delivered asynchronously on the owning GUI thread.
    bool after_pending(Work work, Completion completion);
    QJsonObject snapshot() const;
    void record_external_loss(const QString& error);
    static bool write_status(const QString& path, const QJsonObject& status, QString* error);

private:
    struct QueuedLog { std::uint64_t sequence; std::size_t bytes; LogRecord record; };
    struct Status { std::uint64_t version; QString path; StatusFactory make; };
    struct Barrier { std::uint64_t log_sequence, status_version; Work work; Completion completion; };
    void run();
    void failure_locked(const QString& error);
    LogSink log_sink_;
    StatusSink status_sink_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<QueuedLog> logs_;
    std::optional<Status> pending_status_;
    std::deque<Barrier> barriers_;
    std::thread worker_;
    bool stopping_ = false;
    std::size_t log_bytes_ = 0, log_peak_ = 0;
    std::uint64_t accepted_logs_ = 0, completed_logs_ = 0, dropped_logs_ = 0, dropped_bytes_ = 0;
    std::uint64_t submitted_version_ = 0, completed_version_ = 0, persisted_version_ = 0, writing_version_ = 0;
    std::uint64_t coalesced_status_ = 0, failures_ = 0;
    QString last_error_;
};

}  // namespace redclaw::ui
