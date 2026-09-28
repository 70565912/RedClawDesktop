#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QTimer>
#include <functional>
#include <algorithm>

namespace redclaw::ui {

// Explicit local Debug fixture. Idle until requested, bounded to one 90 s run.
// The same payload/rate is used with either log renderer and the normal sink.
class RuntimeLogReplay final : public QObject {
public:
    RuntimeLogReplay(QObject* parent, std::function<void(const QString&)> append)
        : QObject(parent), append_(std::move(append)) {
        timer_.setInterval(50);
        timer_.setTimerType(Qt::PreciseTimer);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] {
            if (elapsed_.elapsed() >= 90000) { timer_.stop(); return; }
            const auto due = static_cast<quint64>(elapsed_.elapsed() / 50) * 4;
            const auto end = std::min(due, sequence_ + 16);
            while (sequence_ < end) append_(line(sequence_++));
        });
    }
    static QString line(quint64 sequence) {
        return QString("QA_LOG_REPLAY_V1 sequence=%1 ").arg(sequence, 6, 10, QLatin1Char('0'))
            + QStringLiteral("fixed text AaBb 0123456789 capture encode send decode present ").repeated(4);
    }
    void start() { sequence_ = 0; elapsed_.restart(); timer_.start(); }
    void stop() { timer_.stop(); }
    QJsonObject snapshot() const {
        return {{"schema", "redclaw.log-replay.v1"}, {"active", timer_.isActive()},
            {"lines", static_cast<qint64>(sequence_)}, {"interval_ms", 50},
            {"lines_per_tick", 4}, {"duration_limit_ms", 90000}};
    }
private:
    QTimer timer_;
    QElapsedTimer elapsed_;
    quint64 sequence_ = 0;
    std::function<void(const QString&)> append_;
};

}
