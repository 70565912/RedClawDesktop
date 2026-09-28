#include <gtest/gtest.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTimer>
#include <QTextBlock>
#include <atomic>
#include <thread>
#include "redclaw/diag/operation_timing.h"
#include "ui/runtime_log_view.h"

namespace {
class PaintMeasuredLog final : public redclaw::ui::RuntimeLogView {
public:
    qint64 paint_total_us = 0, paint_max_us = 0;
    unsigned paints = 0;
protected:
    void paintEvent(QPaintEvent* event) override {
        QElapsedTimer clock; clock.start();
        QPlainTextEdit::paintEvent(event);
        const auto us = clock.nsecsElapsed() / 1000;
        paint_total_us += us; paint_max_us = std::max(paint_max_us, us); ++paints;
    }
};

TEST(RuntimeLogView, DiagnosticPaintReplay) {
    // Explicit local replay; the source is an already-redacted runtime log.
    // Keep this diagnostic out of ordinary CI runs and never print its contents.
    const auto path = qEnvironmentVariable("REDCLAW_QA_LOG_PAINT_SOURCE");
    if (path.isEmpty()) GTEST_SKIP() << "Explicit local log replay only.";
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const auto lines = QString::fromUtf8(file.readAll()).replace("\r\n", "\n").split('\n');
    ASSERT_GT(lines.size(), 100);
    const auto previous_style = qApp->styleSheet();
    const auto style_path = qEnvironmentVariable("REDCLAW_QA_LOG_PAINT_STYLE");
    if (!style_path.isEmpty()) {
        QFile style(style_path);
        ASSERT_TRUE(style.open(QIODevice::ReadOnly));
        qApp->setStyleSheet(QString::fromUtf8(style.readAll()));
    }
    const bool legacy = qEnvironmentVariableIntValue("REDCLAW_QA_LOG_PAINT_LEGACY") != 0;
    const int seconds = std::clamp(qEnvironmentVariableIntValue("REDCLAW_QA_LOG_PAINT_SECONDS"), 3, 60);
    const int warmup_ms = std::clamp(qEnvironmentVariableIntValue("REDCLAW_QA_LOG_PAINT_WARMUP_MS"), 0, 10000);
    for (const bool shared : {true}) {
        PaintMeasuredLog visible;
        bool height_ok = false;
        const auto requested_height = qEnvironmentVariableIntValue("REDCLAW_QA_LOG_PAINT_HEIGHT", &height_ok);
        visible.resize(800, height_ok ? std::clamp(requested_height, 100, 1000) : 100);
        if (style_path.isEmpty())
            visible.setStyleSheet("QPlainTextEdit { background:#0b1220; color:#e2e8f0; border:1px solid #243244; border-radius:12px; padding:8px 10px; }");
        redclaw::ui::RuntimeLogView mirror;
        if (legacy) mirror.setDocument(visible.document());
        else mirror.set_buffer(visible.buffer());
        QString expected = lines.mid(0, lines.size() / 2).join('\n');
        if (legacy) visible.setPlainText(expected);
        else visible.appendPlainText(expected);
        visible.show();
        if (!legacy) for (int i = 0; i < 100 && visible.toPlainText() != expected; ++i) visible.flush_pending();
        visible.verticalScrollBar()->setValue(visible.verticalScrollBar()->maximum());
        QApplication::processEvents();
        if (warmup_ms) {
            QEventLoop warmup;
            QTimer::singleShot(warmup_ms, &warmup, &QEventLoop::quit);
            warmup.exec();
        }
        visible.paint_total_us = visible.paint_max_us = 0; visible.paints = 0;
        QEventLoop loop;
        QTimer append, heartbeat;
        QElapsedTimer clock; clock.start();
        qint64 previous = 0, maximum_gap = 0;
        qsizetype offset = lines.size() / 2;
        unsigned batches = 0;
        struct DispatchEvent : QEvent {
            DispatchEvent() : QEvent(QEvent::User), start(redclaw::diag::monotonic_time_us()) {}
            std::uint64_t start;
        };
        struct Receiver : QObject {
            std::vector<std::uint64_t> waits;
            std::atomic_bool pending = false;
            bool event(QEvent* event) override {
                if (event->type() != QEvent::User) return QObject::event(event);
                waits.push_back(redclaw::diag::monotonic_time_us() - static_cast<DispatchEvent*>(event)->start);
                pending = false;
                return true;
            }
        } receiver;
        const auto cpu_begin = redclaw::diag::current_thread_cpu_us();
        std::jthread dispatch([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (!receiver.pending.exchange(true)) QApplication::postEvent(&receiver, new DispatchEvent, Qt::HighEventPriority);
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
        });
        QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&] {
            const auto now = clock.elapsed();
            maximum_gap = std::max(maximum_gap, now - previous); previous = now;
        });
        QObject::connect(&append, &QTimer::timeout, &loop, [&] {
            QStringList batch;
            const auto due = std::min(static_cast<unsigned>(seconds * 20), static_cast<unsigned>(clock.elapsed() / 50));
            const auto count = std::min(4U, due - batches);
            for (unsigned i = 0; i < count * 4; ++i) batch.append(lines[offset++ % lines.size()]);
            if (batch.isEmpty()) return;
            if (legacy) visible.QPlainTextEdit::appendPlainText(batch.join('\n'));
            else visible.appendPlainText(batch.join('\n'));
            expected += '\n' + batch.join('\n');
            batches += count;
            if (batches == static_cast<unsigned>(seconds * 20)) loop.quit();
        });
        append.setTimerType(Qt::PreciseTimer);
        append.start(50); heartbeat.start(5);
        QTimer::singleShot((seconds + 10) * 1000, &loop, &QEventLoop::quit);
        loop.exec();
        const auto cpu_us = redclaw::diag::current_thread_cpu_us() - cpu_begin;
        dispatch.request_stop(); dispatch.join();
        std::sort(receiver.waits.begin(), receiver.waits.end());
        const auto percentile = [&](std::size_t p) {
            return receiver.waits.empty() ? 0ULL : receiver.waits[(receiver.waits.size() * p + 99) / 100 - 1];
        };
        std::fprintf(stderr, "log_replay legacy=%d shared=%d batches=%u paint_count=%u paint_total_us=%lld paint_max_us=%lld heartbeat_max_ms=%lld gui_cpu_us=%llu dispatch_count=%zu dispatch_p95_us=%llu dispatch_p99_us=%llu\n",
            legacy, shared, batches, visible.paints, visible.paint_total_us, visible.paint_max_us, maximum_gap,
            static_cast<unsigned long long>(cpu_us), receiver.waits.size(), percentile(95), percentile(99));
        if (legacy) {
            const auto retained = expected.split('\n');
            expected = retained.mid(std::max(qsizetype{0}, retained.size() - 4096)).join('\n');
        }
        else expected = visible.cached_text();
        if (!legacy) for (int i = 0; i < 100 && visible.toPlainText() != expected; ++i) visible.flush_pending();
        EXPECT_EQ(batches, static_cast<unsigned>(seconds * 20));
        EXPECT_LT(maximum_gap, 250);
        EXPECT_EQ(visible.toPlainText(), expected);
        if (shared && !legacy) {
            EXPECT_EQ(mirror.cached_text(), expected);
            EXPECT_TRUE(mirror.toPlainText().isEmpty());
            EXPECT_EQ(mirror.refresh_count(), 0U);
        }
    }
    qApp->setStyleSheet(previous_style);
}

void drain_log(redclaw::ui::RuntimeLogView& view) {
    for (int i = 0; i < 300 && view.toPlainText() != view.cached_text(); ++i) view.flush_pending();
    EXPECT_EQ(view.toPlainText(), view.cached_text());
}

TEST(RuntimeLogView, HiddenDualViewsShareOnlyTextAndClearPending) {
    redclaw::ui::RuntimeLogView first, second;
    second.set_buffer(first.buffer());
    ASSERT_NE(first.document(), second.document());
    int changes = 0;
    QObject::connect(second.document(), &QTextDocument::contentsChanged, [&] { ++changes; });
    first.appendPlainText("one\ntwo\n");
    first.flush_pending(); second.flush_pending();
    EXPECT_EQ(changes, 0);
    EXPECT_TRUE(first.toPlainText().isEmpty());
    EXPECT_EQ(second.cached_text(), QString("one\ntwo\n"));
    first.show(); first.flush_pending();
    EXPECT_EQ(first.toPlainText(), first.cached_text());
    EXPECT_EQ(changes, 0);
    second.show(); second.flush_pending();
    EXPECT_EQ(second.toPlainText(), first.toPlainText());
    first.appendPlainText("not displayed yet");
    first.clear_cached();
    EXPECT_TRUE(first.cached_text().isEmpty());
    EXPECT_TRUE(first.toPlainText().isEmpty());
    EXPECT_TRUE(second.toPlainText().isEmpty());
    first.appendPlainText("after clear");
    first.flush_pending(); second.flush_pending();
    EXPECT_EQ(second.toPlainText(), QString("after clear"));
}

TEST(RuntimeLogView, BoundedCatchupAndEvictionKeepRecentRecords) {
    redclaw::ui::RuntimeLogView view;
    for (int i = 0; i < 5000; ++i) view.appendPlainText(QString::number(i));
    EXPECT_EQ(view.buffer()->line_count(), 4096);
    EXPECT_TRUE(view.cached_text().startsWith("904\n"));
    view.show(); view.flush_pending();
    EXPECT_EQ(view.document()->blockCount(), 64);
    drain_log(view);
    view.hide();
    for (int i = 5000; i < 5100; ++i) view.appendPlainText(QString::number(i));
    const auto before = view.refresh_count();
    view.flush_pending();
    EXPECT_EQ(view.refresh_count(), before);
    view.show(); drain_log(view);
    EXPECT_TRUE(view.toPlainText().startsWith("1004\n"));
    EXPECT_TRUE(view.toPlainText().endsWith("5099"));
}

TEST(RuntimeLogView, LongLinesRespectByteBudgetAndSurrogates) {
    redclaw::ui::RuntimeLogView view;
    const QString astral = QString::fromUtf8("\xF0\x9F\x8C\x8D");
    const QString line = QString(16382, 'x') + astral + QString(40000, 'y');
    view.appendPlainText(line);
    view.show(); view.flush_pending();
    EXPECT_LE(view.toPlainText().size() * 2, view.kBatchBytes);
    EXPECT_FALSE(view.toPlainText().back().isHighSurrogate());
    drain_log(view);
    EXPECT_EQ(view.toPlainText(), line);
    view.appendPlainText(QString(3 * 1024 * 1024, 'z'));
    EXPECT_LE(view.buffer()->bytes(), redclaw::ui::RuntimeLogBuffer::kMaxBytes);
    EXPECT_EQ(view.buffer()->line_count(), 1);
    // Old document contents are discarded before bounded catchup, even when a
    // single new line evicts the entire previously displayed cache.
    view.flush_pending();
    EXPECT_LE(view.toPlainText().size() * 2, view.kBatchBytes);
}

TEST(RuntimeLogView, HistoryPositionAndTailFollowAreIndependent) {
    redclaw::ui::RuntimeLogView view;
    view.resize(500, 100);
    for (int i = 0; i < 200; ++i) view.appendPlainText(QString("record %1").arg(i));
    view.show(); drain_log(view);
    QApplication::processEvents();
    auto* scroll = view.verticalScrollBar();
    scroll->setValue(30);
    const auto history = scroll->value();
    view.appendPlainText("new record"); view.flush_pending();
    EXPECT_EQ(scroll->value(), history);
    scroll->setValue(scroll->maximum());
    view.appendPlainText("tail record"); view.flush_pending();
    EXPECT_EQ(scroll->value(), scroll->maximum());
}

TEST(RuntimeLogView, EmptyRecordsSurviveSeparateFlushes) {
    redclaw::ui::RuntimeLogView view;
    view.show();
    for (const auto& text : {QString(), QString(), QString("content"), QString()}) {
        view.appendPlainText(text);
        view.flush_pending();
        EXPECT_EQ(view.toPlainText(), view.cached_text());
    }
    EXPECT_EQ(view.toPlainText(), QString("\n\ncontent\n"));
}

TEST(RuntimeLogView, WrappedHistorySurvivesEviction) {
    class HistoryView : public redclaw::ui::RuntimeLogView {
    public:
        QString first_text() const { return firstVisibleBlock().text(); }
    } view;
    view.resize(360, 100);
    for (int i = 0; i < 4096; ++i) view.appendPlainText(QString("record %1 ").arg(i) + QString(90, 'x'));
    view.show(); drain_log(view);
    QApplication::processEvents();
    view.verticalScrollBar()->setValue(200);
    QApplication::processEvents();
    const auto original = view.first_text();
    for (int i = 0; i < 10; ++i) view.appendPlainText(QString("new %1 ").arg(i) + QString(90, 'y'));
    view.flush_pending();
    EXPECT_EQ(view.first_text(), original);
}
}
