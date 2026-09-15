#include <gtest/gtest.h>
#include "ui/terminal/terminal_view.h"
#include "ui/terminal/workspace_pipe_server.h"
#include "ui/terminal/terminal_panel.h"
#include "redclaw/workspace/local_workspace_pipe.h"
#include "redclaw/workspace/terminal_session.h"
#include "redclaw/workspace/terminal_runtime_bridge.h"

#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QVBoxLayout>
#include <QToolButton>
#include <QSplitter>
#include <QLabel>
#ifdef _WIN32
#include <Windows.h>
#endif

#include <functional>

namespace {
bool spin_until(const std::function<bool()>& condition, int milliseconds = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < milliseconds) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    return condition();
}

TEST(TerminalBridgeIntegration, RealKeyboardThroughPanelPipeAndWirePreservesShellOnReconnect) {
    const auto runtime = qEnvironmentVariable("REDCLAW_TEST_TERMINAL_RUNTIME");
    if (runtime.isEmpty() || !qEnvironmentVariableIsSet("REDCLAW_TEST_TERMINAL_KEYBOARD")) {
        GTEST_SKIP() << "Requires verified fixed runtime and opt-in input to this test's own foreground window.";
    }
#ifdef _WIN32
    using namespace redclaw::protocol;
    redclaw::ui::WorkspacePipeServer server;
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    QWidget window;
    window.setWindowTitle("RedClaw terminal pipeline validation");
    window.setWindowFlag(Qt::WindowStaysOnTopHint);
    window.resize(1100, 700);
    auto* layout = new QVBoxLayout(&window);
    auto* panel = new redclaw::ui::TerminalPanel(server, nullptr, &window, runtime, profile.path());
    layout->addWidget(panel);
    auto* view = static_cast<redclaw::ui::TerminalView*>(panel->findChild<QWidget*>("remoteTerminalView"));
    auto* toggle = panel->findChild<QToolButton*>("terminalExpandButton");
    ASSERT_TRUE(view && toggle);
    const auto pipe_name = server.listen();
    ASSERT_FALSE(pipe_name.isEmpty());
    server.set_expected_runtime_pid(static_cast<quint32>(QCoreApplication::applicationPid()));
    redclaw::workspace::TerminalRuntimeBridge* receiver = nullptr;
    bool permitted = true, online = true;
    std::string host_output, terminal_id;
    std::uint64_t acknowledgments = 0, input_messages = 0, open_messages = 0, ready_messages = 0;
    redclaw::workspace::TerminalRuntimeBridge host(true, "pipeline-host-1", std::filesystem::current_path(),
        [&](std::string_view frame) {
            const auto parsed = parse_terminal_message_v1(frame);
            if (parsed.ok) {
                if (parsed.value.type == TerminalMessageTypeV1::kOutput) host_output += parsed.value.bytes;
                if (parsed.value.type == TerminalMessageTypeV1::kReady) { terminal_id = parsed.value.terminal_id; ++ready_messages; }
            }
            return online && receiver && receiver->receive(frame);
        }, [] { return true; }, [&] { return permitted; });
    redclaw::workspace::TerminalRuntimeBridge controller(false, "pipeline-controller-1", {},
        [&](std::string_view frame) {
            const auto parsed = parse_terminal_message_v1(frame);
            if (parsed.ok && parsed.value.type == TerminalMessageTypeV1::kOutputAck) ++acknowledgments;
            if (parsed.ok && parsed.value.type == TerminalMessageTypeV1::kInput) ++input_messages;
            if (parsed.ok && parsed.value.type == TerminalMessageTypeV1::kOpen) ++open_messages;
            return online && host.receive(frame);
        }, [] { return true; }, [] { return false; });
    receiver = &controller;
    const auto old_name = qgetenv("REDCLAW_WORKSPACE_PIPE_NAME"), old_owner = qgetenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID");
    qputenv("REDCLAW_WORKSPACE_PIPE_NAME", pipe_name.toUtf8());
    qputenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID", QByteArray::number(QCoreApplication::applicationPid()));
    controller.connect_gui_from_environment();
    if (old_name.isNull()) qunsetenv("REDCLAW_WORKSPACE_PIPE_NAME"); else qputenv("REDCLAW_WORKSPACE_PIPE_NAME", old_name);
    if (old_owner.isNull()) qunsetenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID"); else qputenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID", old_owner);
    host.peer_capability(1, "pipeline-controller-1"); controller.peer_capability(1, "pipeline-host-1");
    host.channel_open(true); controller.channel_open(true);
    QElapsedTimer clock; clock.start();
    const auto pump = [&] {
        host.pump(static_cast<std::uint64_t>(clock.elapsed()), online, permitted);
        controller.pump(static_cast<std::uint64_t>(clock.elapsed()), online);
    };
    const auto wait_for = [&](const std::function<bool()>& condition, int timeout = 15000) {
        return spin_until([&] { pump(); return condition(); }, timeout);
    };
    window.show(); window.activateWindow();
    toggle->click();
    ASSERT_TRUE(wait_for([&] { return view->ready() && !terminal_id.empty() && acknowledgments > 0; }));
    const std::string original_terminal = terminal_id;
    std::string keyboard_error;
    const auto type_command = [&](const std::wstring& command) {
        window.activateWindow(); view->setFocus();
        SetForegroundWindow(reinterpret_cast<HWND>(window.winId()));
        if (GetForegroundWindow() != reinterpret_cast<HWND>(window.winId())) {
            // Windows may deny background SetForegroundWindow. Activate only
            // this fixture's visible title bar, never whichever app has focus.
            RECT bounds{};
            const auto own_window = reinterpret_cast<HWND>(window.winId());
            if (!GetWindowRect(own_window, &bounds)) return false;
            const POINT title{bounds.left + 60, bounds.top + 15};
            if (WindowFromPoint(title) != own_window || !SetCursorPos(title.x, title.y)) {
                keyboard_error = "own_window_activation_occluded"; return false;
            }
            INPUT click[2]{}; click[0].type = click[1].type = INPUT_MOUSE;
            click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            if (SendInput(2, click, sizeof(INPUT)) != 2) return false;
        }
        if (!wait_for([&] { return GetForegroundWindow() == reinterpret_cast<HWND>(window.winId()); }, 2000)) {
            keyboard_error = "own_window_not_foreground"; return false;
        }
        view->clearFocus(); view->setFocus();
        const auto focus_by = clock.elapsed() + 100;
        if (!wait_for([&] { return clock.elapsed() >= focus_by; }, 500)) return false;
        for (const wchar_t ch : command) {
            if (GetForegroundWindow() != reinterpret_cast<HWND>(window.winId())) {
                keyboard_error = "own_window_lost_foreground"; return false;
            }
            INPUT input[2]{};
            input[0].type = input[1].type = INPUT_KEYBOARD;
            input[0].ki.wScan = input[1].ki.wScan = ch;
            input[0].ki.dwFlags = KEYEVENTF_UNICODE; input[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            if (SendInput(2, input, sizeof(INPUT)) != 2) {
                keyboard_error = "send_input_failed:" + std::to_string(GetLastError()); return false;
            }
            QApplication::processEvents(QEventLoop::AllEvents, 5); pump();
        }
        INPUT enter[2]{}; enter[0].type = enter[1].type = INPUT_KEYBOARD;
        enter[0].ki.wVk = enter[1].ki.wVk = VK_RETURN; enter[1].ki.dwFlags = KEYEVENTF_KEYUP;
        return SendInput(2, enter, sizeof(INPUT)) == 2;
    };
    ASSERT_TRUE(type_command(L"$redclawMarker=42; Write-Output ('PIPE_READY='+$redclawMarker)")) << keyboard_error;
    ASSERT_TRUE(wait_for([&] { return host_output.find("PIPE_READY=42") != std::string::npos; }))
        << "input messages=" << input_messages << " output=" << host_output;
    ASSERT_GT(input_messages, 0U);
    // Keep the pane expanded: reconnect must not depend on a later toggle or
    // showEvent to reopen its transport while retaining the Host shell.
    EXPECT_TRUE(view->isVisible());
    online = false; host.channel_open(false); controller.channel_open(false);
    pump();
    host.reset_transport("pipeline-host-2"); controller.reset_transport("pipeline-controller-2");
    host.peer_capability(1, "pipeline-controller-2"); controller.peer_capability(1, "pipeline-host-2");
    host.channel_open(true); controller.channel_open(true); online = true;
    ASSERT_TRUE(wait_for([&] { return view->ready() && terminal_id == original_terminal; }));
    // Allow Ready's input generation to traverse the local pipe before typing.
    const auto ready_by = clock.elapsed() + 500;
    ASSERT_TRUE(wait_for([&] { return clock.elapsed() >= ready_by; }));
    ASSERT_TRUE(type_command(L"Write-Output ('AFTER_RECONNECT='+$redclawMarker); Write-Output '\u4e2d\u6587\u7ec8\u7aef'"));
    ASSERT_TRUE(wait_for([&] { return host_output.find("AFTER_RECONNECT=42") != std::string::npos; })) << host_output;
    EXPECT_EQ(terminal_id, original_terminal);

    // A Controller runtime replacement also reconnects the GUI pipe. The
    // desktop and client identity survive, so this is not a terminal End.
    const auto replacement_pipe = server.listen();
    ASSERT_FALSE(replacement_pipe.isEmpty());
    server.set_expected_runtime_pid(static_cast<quint32>(QCoreApplication::applicationPid()));
    qputenv("REDCLAW_WORKSPACE_PIPE_NAME", replacement_pipe.toUtf8());
    qputenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID", QByteArray::number(QCoreApplication::applicationPid()));
    controller.connect_gui_from_environment();
    if (old_name.isNull()) qunsetenv("REDCLAW_WORKSPACE_PIPE_NAME"); else qputenv("REDCLAW_WORKSPACE_PIPE_NAME", old_name);
    if (old_owner.isNull()) qunsetenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID"); else qputenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID", old_owner);
    controller.reset_transport("pipeline-controller-3");
    controller.peer_capability(1, "pipeline-host-2"); controller.channel_open(true);
    const auto pipe_ready_by = clock.elapsed() + 500;
    ASSERT_TRUE(wait_for([&] {
        const auto labels = panel->findChildren<QLabel*>();
        return server.connected() && clock.elapsed() >= pipe_ready_by
            && std::any_of(labels.begin(), labels.end(), [](auto* label) { return label->text() == QString::fromUtf8("对端 PowerShell"); });
    })) << "opens=" << open_messages << " ready=" << ready_messages;
    const auto pipe_inputs_before = input_messages;
    ASSERT_TRUE(type_command(L"Write-Output ('AFTER_PIPE_RECONNECT='+$redclawMarker)"));
    ASSERT_TRUE(wait_for([&] { return host_output.find("AFTER_PIPE_RECONNECT=42") != std::string::npos; }))
        << "inputs=" << input_messages << " before=" << pipe_inputs_before
        << " opens=" << open_messages << " ready=" << ready_messages;
    EXPECT_EQ(terminal_id, original_terminal);

    // Explicitly ending the desktop still cleans up the shell. Reopening its
    // pipe with the pane visible must create a fresh shell without old input.
    bool ended = false;
    panel->end_desktop([&] { ended = true; });
    ASSERT_TRUE(wait_for([&] { return ended; }));
    const auto fresh_pipe = server.listen();
    server.set_expected_runtime_pid(static_cast<quint32>(QCoreApplication::applicationPid()));
    qputenv("REDCLAW_WORKSPACE_PIPE_NAME", fresh_pipe.toUtf8());
    qputenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID", QByteArray::number(QCoreApplication::applicationPid()));
    controller.connect_gui_from_environment();
    if (old_name.isNull()) qunsetenv("REDCLAW_WORKSPACE_PIPE_NAME"); else qputenv("REDCLAW_WORKSPACE_PIPE_NAME", old_name);
    if (old_owner.isNull()) qunsetenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID"); else qputenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID", old_owner);
    controller.reset_transport("pipeline-controller-4");
    controller.peer_capability(1, "pipeline-host-2"); controller.channel_open(true);
    ASSERT_TRUE(wait_for([&] { return terminal_id != original_terminal && view->ready(); }));
    const auto fresh_ready_by = clock.elapsed() + 500;
    ASSERT_TRUE(wait_for([&] { return clock.elapsed() >= fresh_ready_by; }));
    ASSERT_TRUE(type_command(L"Write-Output ('FRESH_SHELL='+[bool](Get-Variable redclawMarker -ErrorAction Ignore))"));
    ASSERT_TRUE(wait_for([&] { return host_output.find("FRESH_SHELL=False") != std::string::npos; })) << host_output;
    permitted = false;
    const auto paused_by = clock.elapsed() + 600;
    ASSERT_TRUE(wait_for([&] { return clock.elapsed() >= paused_by; }));
    const auto before = input_messages;
    ASSERT_TRUE(type_command(L"Write-Output 'MUST_NOT_EXECUTE'"));
    const auto checked_by = clock.elapsed() + 200;
    ASSERT_TRUE(wait_for([&] { return clock.elapsed() >= checked_by; }));
    EXPECT_EQ(input_messages, before);
    EXPECT_EQ(host_output.find("MUST_NOT_EXECUTE"), std::string::npos);
    const auto evidence = qEnvironmentVariable("REDCLAW_TERMINAL_PIPELINE_RECEIPT");
    if (!evidence.isEmpty()) {
        QFile file(evidence); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{"native_window", QString::number(window.winId())},
            {"output_acknowledgments", static_cast<qint64>(acknowledgments)}, {"input_messages", static_cast<qint64>(input_messages)},
            {"fresh_terminal_after_explicit_end", terminal_id != original_terminal}, {"blocked_input_unchanged", input_messages == before}}).toJson());
    }
    const auto hold = qEnvironmentVariableIntValue("REDCLAW_TERMINAL_VISUAL_HOLD_MS");
    if (hold > 0) { const auto end = clock.elapsed() + hold; (void)wait_for([&] { return clock.elapsed() >= end; }, hold + 100); }
#endif
}

TEST(TerminalPanelLayout, CollapsedPaneGivesSpaceBackToDesktop) {
    redclaw::ui::WorkspacePipeServer pipe;
    QWidget window;
    window.resize(800, 600);
    auto* layout = new QVBoxLayout(&window);
    auto* splitter = new QSplitter(Qt::Vertical, &window);
    splitter->setChildrenCollapsible(false);
    auto* desktop = new QWidget(splitter);
    desktop->setMinimumSize(320, 180);
    desktop->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    splitter->addWidget(desktop);
    auto* panel = new redclaw::ui::TerminalPanel(pipe, nullptr, splitter);
    splitter->addWidget(panel);
    splitter->setStretchFactor(0, 1); splitter->setStretchFactor(1, 0);
    splitter->setSizes({600, 240});
    layout->addWidget(splitter);
    window.show();
    ASSERT_TRUE(spin_until([&] { return splitter->height() > 500; }));
    QApplication::processEvents();
    ASSERT_EQ(splitter->count(), 2);
    EXPECT_LE(panel->height(), 40);
    EXPECT_GE(desktop->height(), splitter->height() - 50)
        << "desktop=" << desktop->geometry().height() << " panel=" << panel->geometry().height()
        << " splitter=" << splitter->height();
}

TEST(TerminalBridgeIntegration, FastChannelReopenStillPublishesAnInputBarrier) {
    using namespace redclaw::protocol;
    redclaw::ui::WorkspacePipeServer server;
    const auto name = server.listen();
    ASSERT_FALSE(name.isEmpty());
    server.set_expected_runtime_pid(static_cast<quint32>(QCoreApplication::applicationPid()));
    std::vector<TerminalMessageV1> sent;
    redclaw::workspace::TerminalRuntimeBridge bridge(false, "controller-epoch", {},
        [&](auto frame) {
            const auto parsed = parse_terminal_message_v1(frame);
            EXPECT_TRUE(parsed.ok) << parsed.error;
            if (parsed.ok) sent.push_back(parsed.value);
            return parsed.ok;
        }, [] { return true; }, [] { return false; });
    const auto previous_name = qgetenv("REDCLAW_WORKSPACE_PIPE_NAME");
    const auto previous_owner = qgetenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID");
    qputenv("REDCLAW_WORKSPACE_PIPE_NAME", name.toUtf8());
    qputenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID", QByteArray::number(QCoreApplication::applicationPid()));
    bridge.connect_gui_from_environment();
    if (previous_name.isNull()) qunsetenv("REDCLAW_WORKSPACE_PIPE_NAME"); else qputenv("REDCLAW_WORKSPACE_PIPE_NAME", previous_name);
    if (previous_owner.isNull()) qunsetenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID"); else qputenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID", previous_owner);
    std::vector<bool> availability;
    bool local_paused = false;
    std::vector<bool> pauses;
    unsigned output_count = 0;
    server.set_receive_callback([&](auto frame) {
        const auto parsed = redclaw::protocol::parse_terminal_message_v1(frame);
        ASSERT_TRUE(parsed.ok) << parsed.error;
        if (parsed.ok && parsed.value.type == redclaw::protocol::TerminalMessageTypeV1::kAvailability) {
            availability.push_back(parsed.value.input_enabled);
            local_paused = parsed.value.local_input_paused;
            pauses.push_back(local_paused);
        }
        if (parsed.ok && parsed.value.type == TerminalMessageTypeV1::kOutput) ++output_count;
    });
    bridge.peer_capability(1, "host-epoch"); bridge.channel_open(true);
    QElapsedTimer clock; clock.start();
    bool allowed = true;
    std::uint64_t transfer_revision = 0;
    const auto pump = [&] { bridge.pump(static_cast<std::uint64_t>(clock.elapsed()), true, allowed, transfer_revision); };
    ASSERT_TRUE(spin_until([&] { pump(); return !availability.empty(); }));
    ASSERT_EQ(availability, std::vector<bool>({true}));
    bridge.channel_open(false);
    bridge.channel_open(true); // both events occur before the next owner tick
    ASSERT_TRUE(spin_until([&] { pump(); return availability.size() >= 3; }));
    EXPECT_EQ(availability, std::vector<bool>({true, false, true}));
    allowed = false;
    ASSERT_TRUE(spin_until([&] { pump(); return local_paused; }));
    EXPECT_TRUE(availability.back()); // the transfer gate does not disconnect terminal output
    TerminalMessageV1 message;
    message.session_epoch = "host-epoch"; message.terminal_id = "shell";
    message.type = TerminalMessageTypeV1::kInput; message.input_generation = 1;
    message.sequence = 1; message.bytes = "late input";
    bool write_completed = false;
    server.set_writable_callback([&] { write_completed = true; });
    ASSERT_TRUE(server.send(serialize_terminal_message_v1(message)));
    ASSERT_TRUE(spin_until([&] { pump(); return write_completed; }));
    message.type = TerminalMessageTypeV1::kOutputAck; message.bytes.clear();
    ASSERT_TRUE(server.send(serialize_terminal_message_v1(message)));
    ASSERT_TRUE(spin_until([&] { pump(); return !sent.empty(); }));
    ASSERT_EQ(sent.size(), 1U); EXPECT_EQ(sent.front().type, TerminalMessageTypeV1::kOutputAck);
    message.type = TerminalMessageTypeV1::kOutput; message.bytes = "background output";
    ASSERT_TRUE(bridge.receive(serialize_terminal_message_v1(message)));
    ASSERT_TRUE(spin_until([&] { pump(); return output_count == 1; }));
    allowed = true;
    ASSERT_TRUE(spin_until([&] { pump(); return !local_paused; }));
    message.type = TerminalMessageTypeV1::kInput; message.bytes = "fresh input";
    ASSERT_TRUE(server.send(serialize_terminal_message_v1(message)));
    ASSERT_TRUE(spin_until([&] { pump(); return sent.size() == 2; }));
    EXPECT_EQ(sent.back().bytes, "fresh input");
    pauses.clear();
    ++transfer_revision; // an entire short transfer happened between owner ticks
    ASSERT_TRUE(spin_until([&] { pump(); return pauses.size() >= 2; }));
    EXPECT_EQ(pauses, std::vector<bool>({true, false}));
}

TEST(WorkspacePipeIntegration, BoundedBinaryExchangeAndOwnerIdentity) {
    redclaw::ui::WorkspacePipeServer server;
    redclaw::workspace::LocalWorkspacePipe client;
    QString error;
    const auto name = server.listen(&error);
    ASSERT_FALSE(name.isEmpty()) << error.toStdString();
    const auto process_id = static_cast<std::uint32_t>(QCoreApplication::applicationPid());
    server.set_expected_runtime_pid(process_id);
    std::string pipe_error;
    EXPECT_FALSE(client.connect(name.toStdString(), process_id + 1, &pipe_error));
    EXPECT_EQ(pipe_error, "workspace_pipe_owner_mismatch");
    server.close();
    const auto valid_name = server.listen(&error);
    server.set_expected_runtime_pid(process_id);
    ASSERT_TRUE(client.connect(valid_name.toStdString(), process_id, &pipe_error)) << pipe_error;
    ASSERT_TRUE(spin_until([&] { return server.connected(); }));
    std::string payload(64 * 1024, '\0');
    for (std::size_t index = 0; index < payload.size(); ++index) payload[index] = static_cast<char>(index % 251);
    std::string received;
    server.set_receive_callback([&](std::string_view frame) { received.assign(frame); });
    ASSERT_TRUE(client.send(payload));
    ASSERT_TRUE(spin_until([&] { client.poll([](std::string_view) {}); return received == payload; }));
    EXPECT_FALSE(client.send(std::string(64 * 1024 + 1, 'x')));
    received.clear();
    ASSERT_TRUE(server.send(payload));
    ASSERT_TRUE(spin_until([&] { client.poll([&](std::string_view frame) { received.assign(frame); }); return received == payload; }));
    server.close();
    ASSERT_TRUE(spin_until([&] { client.poll([](std::string_view) {}); return !client.connected(); }));
    const auto rejected_name = server.listen(&error);
    server.set_expected_runtime_pid(process_id + 1);
    ASSERT_TRUE(client.connect(rejected_name.toStdString(), process_id, &pipe_error));
    ASSERT_TRUE(spin_until([&] { client.poll([](std::string_view) {}); return !client.connected(); }));
    EXPECT_FALSE(server.connected());
}

TEST(TerminalViewIntegration, BundledSurfaceParsesRealShellAndSurvivesCollapse) {
    const auto runtime = qEnvironmentVariable("REDCLAW_TEST_TERMINAL_RUNTIME");
    if (runtime.isEmpty()) GTEST_SKIP() << "Set REDCLAW_TEST_TERMINAL_RUNTIME to the verified fixed runtime directory.";
    QTemporaryDir profile;
    ASSERT_TRUE(profile.isValid());
    redclaw::workspace::TerminalSession terminal;
    std::string error;
    ASSERT_TRUE(terminal.start(std::filesystem::current_path(), {120, 32}, &error)) << error;
    QWidget window;
    window.setWindowTitle("RedClaw terminal validation");
    if (!qEnvironmentVariableIsEmpty("REDCLAW_TERMINAL_VISUAL_RECEIPT")) {
        window.setWindowFlag(Qt::WindowStaysOnTopHint);
    }
    window.resize(1100, 660);
    window.move(80, 80);
    auto* layout = new QVBoxLayout(&window);
    auto* view = new redclaw::ui::TerminalView(&window);
    layout->addWidget(view);
    QString surface_error;
    int acknowledgments = 0, size_changes = 0;
    std::string received;
    view->set_error_callback([&](const QString& message) { surface_error = message; });
    view->set_input_callback([&](const QByteArray& bytes) { (void)terminal.write(std::string_view(bytes.constData(), bytes.size())); });
    view->set_resize_callback([&](int columns, int rows) {
        ++size_changes;
        (void)terminal.resize({static_cast<std::uint16_t>(columns), static_cast<std::uint16_t>(rows)});
    });
    view->set_writable_callback([&] { ++acknowledgments; });
    view->reset_session("isolated-terminal-validation");
    window.show();
    view->initialize(runtime, profile.path());
    ASSERT_TRUE(spin_until([&] { return view->ready() || !surface_error.isEmpty(); })) << "WebView startup timed out";
    ASSERT_TRUE(view->ready()) << surface_error.toStdString();
    view->set_input_enabled(true);
    ASSERT_TRUE(view->append_output("\x1b[2J\x1b[H"));
    EXPECT_FALSE(view->append_output("must wait for xterm acknowledgment"));
    ASSERT_TRUE(spin_until([&] { return !view->output_pending(); }));
    const auto pump = [&] {
        if (!view->output_pending()) {
            if (auto chunk = terminal.take_output()) {
                received += *chunk;
                EXPECT_TRUE(view->append_output(*chunk));
            }
        }
    };
    ASSERT_TRUE(spin_until([&] { pump(); return received.find("PS ") != std::string::npos; }));
    ASSERT_TRUE(terminal.write("Write-Host ('REAL_'+'POWERSHELL_READY') -ForegroundColor Green; Write-Output ([char]0x4e2d+[string][char]0x6587); $persistentValue=42\r"));
    ASSERT_TRUE(spin_until([&] { pump(); return received.find("REAL_POWERSHELL_READY") != std::string::npos
        && received.find("\xe4\xb8\xad\xe6\x96\x87") != std::string::npos; }));
    EXPECT_GT(size_changes, 0);
    EXPECT_GT(acknowledgments, 1);
    view->hide();
    QApplication::processEvents();
    EXPECT_TRUE(terminal.running());
    view->show();
    window.resize(1050, 610);
    ASSERT_TRUE(terminal.write("Write-Host ('AFTER_'+'COLLAPSE='+$persistentValue) -ForegroundColor Cyan\r"));
    ASSERT_TRUE(spin_until([&] { pump(); return received.find("AFTER_COLLAPSE=42") != std::string::npos; }));
    ASSERT_TRUE(spin_until([&] { pump(); return !view->output_pending() && terminal.buffered_output_bytes() == 0; }));
    const auto clipboard_before = QApplication::clipboard()->text();
    const auto osc_clipboard = QByteArray("\x1b]52;c;") + QByteArray("REDCLAW_SHOULD_NOT_CHANGE_CLIPBOARD").toBase64() + '\x07';
    ASSERT_TRUE(view->append_output(std::string_view(osc_clipboard.constData(), osc_clipboard.size())));
    ASSERT_TRUE(spin_until([&] { return !view->output_pending(); }));
    EXPECT_EQ(QApplication::clipboard()->text(), clipboard_before);
    view->set_input_enabled(false);
    const auto evidence_path = qEnvironmentVariable("REDCLAW_TERMINAL_VISUAL_RECEIPT");
    if (!evidence_path.isEmpty()) {
        QFile receipt(evidence_path);
        ASSERT_TRUE(receipt.open(QIODevice::WriteOnly));
        receipt.write(QJsonDocument(QJsonObject{{"ready", true}, {"native_window", QString::number(window.winId())},
            {"output_acknowledgments", acknowledgments}, {"size_changes", size_changes},
            {"shell_marker", "REAL_POWERSHELL_READY"}, {"preserved_marker", "AFTER_COLLAPSE=42"}}).toJson());
    }
    const int hold_ms = qEnvironmentVariableIntValue("REDCLAW_TERMINAL_VISUAL_HOLD_MS");
    if (hold_ms > 0) {
        QElapsedTimer hold;
        hold.start();
        (void)spin_until([&] { pump(); return hold.elapsed() >= hold_ms; }, hold_ms + 100);
    }
    ASSERT_TRUE(view->append_output("previous session pending output"));
    view->reset_session("new-isolated-terminal-validation");
    EXPECT_FALSE(view->append_output("must wait for reset acknowledgment"));
    ASSERT_TRUE(spin_until([&] { return view->ready(); }));
    ASSERT_TRUE(view->append_output("new session output"));
    ASSERT_TRUE(spin_until([&] { return !view->output_pending(); }));
    terminal.stop();
    EXPECT_TRUE(surface_error.isEmpty()) << surface_error.toStdString();
}
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
