#include <gtest/gtest.h>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTimer>
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
    const auto lines = QString::fromUtf8(file.readAll()).split('\n');
    ASSERT_GT(lines.size(), 100);
    const auto previous_style = qApp->styleSheet();
    const auto style_path = qEnvironmentVariable("REDCLAW_QA_LOG_PAINT_STYLE");
    if (!style_path.isEmpty()) {
        QFile style(style_path);
        ASSERT_TRUE(style.open(QIODevice::ReadOnly));
        qApp->setStyleSheet(QString::fromUtf8(style.readAll()));
    }
    for (const bool shared : {true, false}) {
        PaintMeasuredLog visible;
        bool height_ok = false;
        const auto requested_height = qEnvironmentVariableIntValue("REDCLAW_QA_LOG_PAINT_HEIGHT", &height_ok);
        visible.resize(800, height_ok ? std::clamp(requested_height, 100, 1000) : 100);
        if (style_path.isEmpty())
            visible.setStyleSheet("QPlainTextEdit { background:#0b1220; color:#e2e8f0; border:1px solid #243244; border-radius:12px; padding:8px 10px; }");
        redclaw::ui::RuntimeLogView mirror;
        if (shared) mirror.setDocument(visible.document());
        QString expected = lines.mid(0, lines.size() / 2).join('\n');
        visible.setPlainText(expected);
        visible.verticalScrollBar()->setValue(visible.verticalScrollBar()->maximum());
        visible.show();
        QApplication::processEvents();
        visible.paint_total_us = visible.paint_max_us = 0; visible.paints = 0;
        QEventLoop loop;
        QTimer append, heartbeat;
        QElapsedTimer clock; clock.start();
        qint64 previous = 0, maximum_gap = 0;
        qsizetype offset = lines.size() / 2;
        unsigned batches = 0;
        QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&] {
            const auto now = clock.elapsed();
            maximum_gap = std::max(maximum_gap, now - previous); previous = now;
        });
        QObject::connect(&append, &QTimer::timeout, &loop, [&] {
            QStringList batch;
            for (unsigned i = 0; i < 4; ++i) batch.append(lines[offset++ % lines.size()]);
            visible.appendPlainText(batch.join('\n'));
            expected += '\n' + batch.join('\n');
            if (++batches == 60) loop.quit();
        });
        append.start(50); heartbeat.start(5);
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        std::fprintf(stderr, "log_replay shared=%d batches=%u paint_count=%u paint_total_us=%lld paint_max_us=%lld heartbeat_max_ms=%lld\n",
            shared, batches, visible.paints, visible.paint_total_us, visible.paint_max_us, maximum_gap);
        EXPECT_EQ(batches, 60U);
        EXPECT_LT(maximum_gap, 250);
        EXPECT_EQ(visible.toPlainText(), expected);
        if (shared) EXPECT_EQ(mirror.toPlainText(), expected);
    }
    qApp->setStyleSheet(previous_style);
}
}
