#include "ui/runtime_control_queue.h"
#include <gtest/gtest.h>
#include "ui/runtime_stdio_reader.h"
#include "ui/runtime_control_writer.h"
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QFile>
#include <iostream>
#include <thread>
#include <chrono>

using redclaw::ui::RuntimeControlQueue;

TEST(RuntimeControlQueue, FragmentedReadsRetainMessageOrderAndAccounting) {
    RuntimeControlQueue queue;
    const QByteArray wire("one\ntwo\nthree\n");
    for (const auto c : wire) ASSERT_TRUE(queue.append(QByteArray(1, c)));
    EXPECT_EQ(queue.messages(), 3); EXPECT_EQ(queue.bytes(), wire.size());
    for (const auto expected : {"one", "two", "three"}) {
        auto frame = queue.take(); ASSERT_TRUE(frame); EXPECT_EQ(*frame, expected);
    }
    EXPECT_EQ(queue.bytes(), 0); EXPECT_FALSE(queue.take());
}

TEST(RuntimeControlQueue, MessageByteAndIndividualFrameOverflowFailClosed) {
    RuntimeControlQueue messages;
    for (int i = 0; i < 256; ++i) ASSERT_TRUE(messages.append("ack\n"));
    EXPECT_FALSE(messages.append("ack\n")); EXPECT_TRUE(messages.failed()); EXPECT_FALSE(messages.take());
    RuntimeControlQueue bytes;
    EXPECT_FALSE(bytes.append(QByteArray(RuntimeControlQueue::kByteCapacity + 1, 'x')));
    EXPECT_TRUE(bytes.failed());
    RuntimeControlQueue frame;
    EXPECT_FALSE(frame.append(QByteArray(65537, 'x')));
    EXPECT_TRUE(frame.failed());
}

TEST(RuntimeControlQueue, FullSizeFrameCanArriveBeforeItsDelimiter) {
    RuntimeControlQueue queue;
    ASSERT_TRUE(queue.append(QByteArray(65536, 'x')));
    EXPECT_FALSE(queue.ready());
    ASSERT_TRUE(queue.append("\n"));
    const auto frame = queue.take(); ASSERT_TRUE(frame); EXPECT_EQ(frame->size(), 65536);
}

int run_runtime_stdio_fixture(const char* mode) {
    using namespace std::chrono_literals;
    if (std::string_view(mode) == "input-receipt") {
        QFile output(qEnvironmentVariable("REDCLAW_QA_CONTROL_RECEIPT"));
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
        std::cout << "ready\n" << std::flush;
        std::string line;
        for (unsigned i = 0; i < 3 && std::getline(std::cin, line); ++i) {
            const auto received = redclaw::ui::gui_monotonic_us();
            const auto parsed = redclaw::protocol::parse_local_runtime_control_frame_v2(line);
            if (!parsed.ok) return 3;
            output.write(QByteArray::number(parsed.value.input_sequence) + ' '
                + QByteArray::number(received) + '\n');
            if (!output.flush()) return 4;
        }
        return 0;
    }
    if (std::string_view(mode) == "overflow") {
        std::cout << std::string(65537, 'x') << std::flush;
        std::this_thread::sleep_for(200ms); return 0;
    }
    std::thread diagnostic([] {
        for (int i = 0; i < 200; ++i) std::cerr << std::string(8000, 'd') << '\n';
    });
    for (int i = 1; i <= 100; ++i) {
        redclaw::protocol::StreamControlMessageV1 message;
        message.type = redclaw::protocol::StreamControlMessageTypeV1::kInputControlStatus;
        message.session_epoch = "local"; message.message_id = i; message.input_sequence = i; message.sent_at_ms = 1;
        auto wire = redclaw::protocol::serialize_local_runtime_control_frame_v2(message, redclaw::ui::gui_monotonic_us()) + '\n';
        const auto split = wire.size() / 3;
        std::cout.write(wire.data(), split); std::cout.flush(); std::this_thread::sleep_for(1ms);
        std::cout.write(wire.data()+split, wire.size()-split); std::cout.flush(); std::this_thread::sleep_for(2ms);
    }
    diagnostic.join(); std::this_thread::sleep_for(1500ms); return 0;
}

#ifdef _WIN32
TEST(RuntimeControlWriter, StartsPipeWriteBeforeGuiReturnsToItsEventLoop) {
    using namespace std::chrono_literals;
    QTemporaryDir directory;
    const auto receipt_path = directory.filePath("received.txt");
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("REDCLAW_QA_CONTROL_RECEIPT", receipt_path);
    process.setProcessEnvironment(environment);
    process.start(QCoreApplication::applicationFilePath(), {"--stdio-fixture", "input-receipt"});
    ASSERT_TRUE(process.waitForStarted());
    ASSERT_TRUE(process.waitForReadyRead());
    ASSERT_EQ(process.readAllStandardOutput().trimmed(), "ready");
    const auto sent = redclaw::ui::gui_monotonic_us();
    for (unsigned i = 1; i <= 3; ++i) {
        redclaw::protocol::StreamControlMessageV1 message;
        message.type = redclaw::protocol::StreamControlMessageTypeV1::kInputStateSync;
        message.session_epoch = "local"; message.message_id = i;
        message.input_sequence = i; message.sent_at_ms = 1; message.desktop_geometry_revision = 1;
        EXPECT_TRUE(redclaw::ui::write_runtime_control_message(process, message));
    }
    // Model the measured layout/paint occupancy without pumping Qt events.
    std::this_thread::sleep_for(180ms);
    QFile receipt(receipt_path);
    ASSERT_TRUE(receipt.open(QIODevice::ReadOnly));
    const auto rows = receipt.readAll().trimmed().split('\n');
    // Cleanup may flush the original delayed write, so snapshot before cleanup.
    (void)process.waitForBytesWritten(1000);
    ASSERT_TRUE(process.waitForFinished(3000));
    EXPECT_EQ(process.exitCode(), 0);
    ASSERT_EQ(rows.size(), 3);
    for (qsizetype i = 0; i < rows.size(); ++i) {
        const auto columns = rows[i].split(' ');
        ASSERT_EQ(columns.size(), 2);
        EXPECT_EQ(columns[0].toUInt(), static_cast<unsigned>(i + 1));
        const auto delay = columns[1].toULongLong() - sent;
        RecordProperty("receipt_" + std::to_string(i + 1) + "_us", delay);
        // Finding every receipt before waitForBytesWritten or any Qt event pump
        // proves that the writer starts independently of the GUI event loop.
        // Keep elapsed time as telemetry; process scheduling is host-dependent.
    }
}
#endif

TEST(RuntimeStdioReader, RealSeparatePipesKeepFragmentedControlOrderedUnderDiagnosticFlood) {
    using namespace std::chrono_literals;
    QProcess process; process.setProcessChannelMode(QProcess::SeparateChannels);
    int messages = 0, diagnostics = 0; bool overflow = false;
    std::uint64_t maximum = 0;
    redclaw::ui::RuntimeStdioReader reader(process, [&](const auto& message, auto sent) {
        EXPECT_EQ(message.input_sequence, ++messages);
        maximum = std::max(maximum, redclaw::ui::gui_monotonic_us() - sent);
    }, [&](const QString&) { ++diagnostics; QElapsedTimer cpu; cpu.start(); while (cpu.nsecsElapsed() < 2000000) {} },
        [&](const QString& error, bool) { if (!error.contains("disconnected")) overflow = true; }, nullptr);
    process.start(QCoreApplication::applicationFilePath(), {"--stdio-fixture", "flood"});
    QElapsedTimer elapsed; elapsed.start();
    while (elapsed.elapsed() < 5000 && (messages < 100 || diagnostics < 200 || process.state() != QProcess::NotRunning)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5); std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(messages, 100); EXPECT_EQ(diagnostics, 200); EXPECT_FALSE(overflow); EXPECT_LT(maximum, 250000);
    EXPECT_EQ(process.state(), QProcess::NotRunning);
}

TEST(RuntimeStdioReader, ActualPipeOversizeFailsControlWithoutDeliveringPartialMessage) {
    QProcess process; process.setProcessChannelMode(QProcess::SeparateChannels);
    bool failed = false; int messages = 0;
    redclaw::ui::RuntimeStdioReader reader(process, [&](const auto&, auto) { ++messages; }, [](const auto&) {},
        [&](const QString& error, bool control) { if (!error.contains("disconnected")) failed |= control; }, nullptr);
    process.start(QCoreApplication::applicationFilePath(), {"--stdio-fixture", "overflow"});
    QElapsedTimer elapsed; elapsed.start();
    while (elapsed.elapsed() < 3000 && process.state() != QProcess::NotRunning) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(failed); EXPECT_EQ(messages, 0); EXPECT_TRUE(reader.snapshot()["control_failed"].toBool());
}
