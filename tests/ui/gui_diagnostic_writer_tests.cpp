#include "ui/gui_diagnostic_writer.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>

using redclaw::ui::GuiDiagnosticWriter;
using namespace std::chrono_literals;

namespace {
bool pump_until(const std::function<bool()>& done, int timeout_ms = 3000) {
    QElapsedTimer elapsed; elapsed.start();
    while (!done() && elapsed.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(1ms);
    }
    return done();
}
}

TEST(GuiDiagnosticWriter, SlowLogAndAtomicCommitKeepHeartbeatAndControlCallbacksResponsive) {
    GuiDiagnosticWriter writer([](const auto&, QString*) { std::this_thread::sleep_for(350ms); return true; },
        [](const auto&, const auto&, QString*) { std::this_thread::sleep_for(350ms); return true; });
    QElapsedTimer elapsed; elapsed.start();
    qint64 last = 0, maximum = 0;
    int control_callbacks = 0;
    QTimer heartbeat, control;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        maximum = std::max(maximum, elapsed.elapsed() - last); last = elapsed.elapsed();
    });
    QObject::connect(&control, &QTimer::timeout, [&] { ++control_callbacks; });
    heartbeat.start(10); control.start(5);
    ASSERT_TRUE(writer.post_log({"test", "line", false}));
    ASSERT_GT(writer.submit_status("injected-sink", [] {
        EXPECT_NE(QThread::currentThread(), QCoreApplication::instance()->thread());
        std::this_thread::sleep_for(350ms);
        return QJsonObject{{"version", 1}};
    }), 0);
    bool completed = false, succeeded = false;
    ASSERT_TRUE(writer.after_pending({}, [&](bool ok, const QString&) { succeeded = ok; completed = true; }));
    ASSERT_TRUE(pump_until([&] { return completed; }));
    EXPECT_TRUE(succeeded);
    EXPECT_GE(control_callbacks, 20);
    EXPECT_LT(maximum, 250);
    EXPECT_GE(elapsed.elapsed(), 700);
}

TEST(GuiDiagnosticWriter, OneInflightAndLatestPendingStatusPreserveVersionBarrier) {
    std::atomic<int> writes{0}, last_value{0};
    GuiDiagnosticWriter writer([](const auto&, QString*) { return true; },
        [&](const auto&, const QJsonObject& status, QString*) {
            ++writes; std::this_thread::sleep_for(100ms);
            last_value = status["value"].toInt(); return true;
        });
    writer.submit_status("sink", [] { return QJsonObject{{"value", 1}}; });
    ASSERT_TRUE(pump_until([&] { return writes.load() == 1; }));
    for (int i = 2; i <= 200; ++i) writer.submit_status("sink", [i] { return QJsonObject{{"value", i}}; });
    EXPECT_EQ(writer.snapshot()["status_submitted_version"].toInteger(), 200);
    EXPECT_EQ(writer.snapshot()["status_persisted_version"].toInteger(), 0);
    bool complete = false;
    ASSERT_TRUE(writer.after_pending({}, [&](bool ok, const QString&) { EXPECT_TRUE(ok); complete = true; }));
    ASSERT_TRUE(pump_until([&] { return complete; }));
    EXPECT_EQ(writes.load(), 2); EXPECT_EQ(last_value.load(), 200);
    EXPECT_EQ(writer.snapshot()["status_persisted_version"].toInteger(), 200);
    EXPECT_EQ(writer.snapshot()["status_coalesced"].toInteger(), 198);
}

TEST(GuiDiagnosticWriter, OverflowIsNonblockingAndInvalidatesExportBarrier) {
    GuiDiagnosticWriter writer([](const auto&, QString*) { std::this_thread::sleep_for(150ms); return true; });
    ASSERT_TRUE(writer.post_log({"test", QByteArray(3 * 1024 * 1024, 'x'), false}));
    QElapsedTimer elapsed; elapsed.start();
    EXPECT_FALSE(writer.post_log({"test", QByteArray(2 * 1024 * 1024, 'y'), false}));
    EXPECT_LT(elapsed.elapsed(), 250);
    EXPECT_EQ(writer.snapshot()["log_dropped"].toInteger(), 1);
    EXPECT_LE(writer.snapshot()["log_queue_peak_bytes"].toInteger(), GuiDiagnosticWriter::kLogCapacity);
    bool complete = false;
    ASSERT_TRUE(writer.after_pending({}, [&](bool ok, const QString& error) {
        EXPECT_FALSE(ok); EXPECT_FALSE(error.isEmpty()); complete = true;
    }));
    ASSERT_TRUE(pump_until([&] { return complete; }));
}

TEST(GuiDiagnosticWriter, AtomicWriteFailureIsVisibleAndNeverClaimsPersistence) {
    QTemporaryDir directory;
    GuiDiagnosticWriter writer([](const auto&, QString*) { return true; });
    ASSERT_GT(writer.submit_status(directory.filePath("missing/status.json"), [] { return QJsonObject{{"value", 1}}; }), 0);
    bool complete = false;
    ASSERT_TRUE(writer.after_pending({}, [&](bool ok, const QString& error) {
        EXPECT_FALSE(ok); EXPECT_FALSE(error.isEmpty()); complete = true;
    }));
    ASSERT_TRUE(pump_until([&] { return complete; }));
    const auto status = writer.snapshot();
    EXPECT_EQ(status["status_persisted_version"].toInteger(), 0);
    EXPECT_EQ(status["failures"].toInteger(), 1);
    EXPECT_FALSE(status["evidence_integrity"].toBool());
}

TEST(GuiDiagnosticWriter, ExportAndExitBarriersObserveOrderedLogsAndCommittedStatus) {
    QTemporaryDir directory;
    std::string lines;
    GuiDiagnosticWriter writer([&](const auto& record, QString*) { lines += record.text.toStdString(); return true; });
    writer.post_log({"test", "first", false});
    writer.post_log({"test", "second", false});
    const auto path = directory.filePath("status.json");
    writer.submit_status(path, [] { return QJsonObject{{"final", true}}; });
    bool complete = false;
    ASSERT_TRUE(writer.after_pending([&](QString*) {
        QFile file(path);
        return lines == "firstsecond" && file.open(QIODevice::ReadOnly) && file.readAll().contains("true");
    }, [&](bool ok, const QString&) { EXPECT_TRUE(ok); complete = true; }));
    ASSERT_TRUE(pump_until([&] { return complete; }));
    EXPECT_EQ(writer.snapshot()["log_completed"].toInteger(), 2);
}
