#include <gtest/gtest.h>
#include "ui/workspace_control_server.h"
#include "ui/transfer_coordinator.h"
#include "ui/file_transfer_panel.h"
#include "ui/terminal/terminal_panel.h"
#include "ui/terminal/terminal_coordinator.h"
#include "ui/terminal/workspace_pipe_server.h"
#include "redclaw/workspace/terminal_runtime_bridge.h"
#include "redclaw/workspace/terminal_shell_integration.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

namespace {
using namespace redclaw::ui;
using namespace redclaw::protocol;
using namespace redclaw::workspace;
bool spin(const std::function<bool()>& done, int timeout = 15000) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < timeout) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        if (done()) return true;
        QThread::msleep(2);
    }
    return done();
}
TEST(WorkspaceControl, VersionIdentityDeduplicationAndBoundedEvents) {
    int calls = 0;
    WorkspaceControlServer server([&](auto, auto, auto id) { ++calls; return QJsonObject{{"ok", true}, {"operation_id", id}}; });
    QJsonObject request{{"version", 1}, {"request_id", "request"}, {"instance_id", server.instance_id()},
        {"operation", "terminal.exec"}, {"parameters", QJsonObject{{"command", "test"}}}};
    const auto first = server.dispatch(request);
    EXPECT_TRUE(first.value("ok").toBool()); EXPECT_EQ(calls, 1);
    EXPECT_EQ(server.dispatch(request), first); EXPECT_EQ(calls, 1);
    request["parameters"] = QJsonObject{{"command", "changed"}};
    EXPECT_EQ(server.dispatch(request).value("error"), "request_id_conflict");
    request["instance_id"] = "old-instance";
    EXPECT_EQ(server.dispatch(request).value("error"), "instance_changed");
    request["version"] = 2;
    EXPECT_EQ(server.dispatch(request).value("error"), "protocol_version_incompatible");
    EXPECT_EQ(calls, 1);
    for (int i = 0; i < 2200; ++i) server.observe("operation", {{"state", "running"}});
    request = {{"version", 1}, {"request_id", "events"}, {"instance_id", server.instance_id()}, {"operation", "events"}, {"parameters", QJsonObject{{"cursor", "0"}}}};
    const auto events = server.dispatch(request);
    EXPECT_TRUE(events.value("gap").toBool()); EXPECT_EQ(events.value("events").toArray().size(), 128);
}
TEST(WorkspaceControl, NamedPipeCurrentUserValidationAndMalformedRequest) {
    WorkspaceControlServer server([](auto, auto, auto) { return QJsonObject{{"ok", true}}; });
    EXPECT_FALSE(WorkspaceControlServer::current_user_client(-1));
    const auto name = "RedClaw.Test.Workspace." + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString error; ASSERT_TRUE(server.start(name, &error)) << error.toStdString();
    QLocalSocket client; client.connectToServer(name);
    ASSERT_TRUE(spin([&] { return client.state() == QLocalSocket::ConnectedState; }));
    client.write("{invalid}\n");
    ASSERT_TRUE(spin([&] { return client.bytesAvailable() > 0; }));
    EXPECT_EQ(QJsonDocument::fromJson(client.readAll()).object().value("error"), "invalid_json");
}
TEST(WorkspaceControl, HiddenFilePanelAndApiShareSelectionProgressAndCancellation) {
    std::vector<StreamControlMessageV1> sent;
    FileTransferPanel panel([&](const auto& value, QString*) { sent.push_back(value); return true; });
    EXPECT_FALSE(panel.isVisible());
    StreamControlMessageV1 message; message.type = StreamControlMessageTypeV1::kWorkspace;
    message.workspace.emplace(); message.workspace->action = WorkspaceActionV1::kAvailability; message.file_transfer_version = 1;
    panel.receive(message);
    const auto result = panel.coordinator().invoke("files.upload",
        {{"sources", QJsonArray{"C:/one", "C:/directory"}}, {"destination", "C:/receive"}}, "api-files");
    ASSERT_TRUE(result.value("ok").toBool()); EXPECT_TRUE(panel.busy());
    ASSERT_EQ(sent.size(), 1U); EXPECT_EQ(sent[0].workspace->conflict, TransferConflictV1::kKeepBoth);
    message.request_id = "api-files"; message.workspace->action = WorkspaceActionV1::kPrepared; panel.receive(message);
    ASSERT_EQ(sent.size(), 2U); EXPECT_EQ(sent.back().workspace->path, "C:/one");
    message.workspace->action = WorkspaceActionV1::kSourceAccepted; message.workspace->accepted_sources = 1; panel.receive(message);
    EXPECT_EQ(sent.back().workspace->path, "C:/directory");
    message.workspace->accepted_sources = 2; panel.receive(message);
    EXPECT_EQ(sent.back().workspace->action, WorkspaceActionV1::kSelectionComplete);
    EXPECT_TRUE(panel.coordinator().invoke("operation.cancel", {}, "api-files").value("ok").toBool());
    EXPECT_EQ(sent.back().workspace->action, WorkspaceActionV1::kCancel);
    panel.runtime_stopped();
    EXPECT_EQ(panel.coordinator().invoke("operation.status", {}, "api-files").value("state"), "unknown");
    EXPECT_FALSE(panel.busy());
}
TEST(TerminalShellIntegration, SplitBoundariesAndOrdinaryEscapeOutputRemainExact) {
    TerminalShellIntegration integration; integration.reset("nonce");
    std::vector<TerminalShellIntegration::Event> events; std::string output;
    const std::string bytes = "before\x1b]777;redclaw-v1;nonce;start;op;0;\aafter\x1b]777;redclaw-v1;nonce;done;op;1;7\a";
    for (char byte : bytes) output += integration.consume({&byte, 1}, [&](const auto& value) { events.push_back(value); });
    EXPECT_EQ(output, "beforeafter"); ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].output_position, 6U); EXPECT_EQ(events[1].output_position, 11U);
    EXPECT_TRUE(events[1].success); EXPECT_EQ(events[1].last_native_exit_code, 7);
    EXPECT_EQ(integration.consume("\x1b[31mred", {}), "\x1b[31mred");
}
TEST(WorkspaceTerminalIntegration, NeverShownPanelRunsSharedShellAndReturnsExplicitCompletion) {
    WorkspacePipeServer pipe; QTemporaryDir profile;
    TerminalPanel panel(pipe, nullptr, {}, profile.path()); EXPECT_FALSE(panel.isVisible());
    const auto name = pipe.listen(); ASSERT_FALSE(name.isEmpty());
    pipe.set_expected_runtime_pid(static_cast<quint32>(QCoreApplication::applicationPid()));
    TerminalRuntimeBridge* controller_pointer = nullptr; bool online = true;
    TerminalRuntimeBridge host(true, "host", std::filesystem::current_path(),
        [&](auto frame) { return online && controller_pointer && controller_pointer->receive(frame); }, [] { return true; }, [] { return true; });
    TerminalRuntimeBridge controller(false, "controller", {},
        [&](auto frame) { return online && host.receive(frame); }, [] { return true; }, [] { return true; });
    controller_pointer = &controller;
    const auto old_name = qgetenv("REDCLAW_WORKSPACE_PIPE_NAME"), old_owner = qgetenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID");
    qputenv("REDCLAW_WORKSPACE_PIPE_NAME", name.toUtf8());
    qputenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID", QByteArray::number(QCoreApplication::applicationPid()));
    controller.connect_gui_from_environment();
    if (old_name.isNull()) qunsetenv("REDCLAW_WORKSPACE_PIPE_NAME"); else qputenv("REDCLAW_WORKSPACE_PIPE_NAME", old_name);
    if (old_owner.isNull()) qunsetenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID"); else qputenv("REDCLAW_WORKSPACE_PIPE_OWNER_PID", old_owner);
    host.peer_capability(2, "controller"); controller.peer_capability(2, "host");
    host.channel_open(true); controller.channel_open(true);
    QElapsedTimer clock; clock.start();
    const auto wait = [&](const std::function<bool()>& condition, int timeout = 15000) {
        return spin([&] { host.pump(clock.elapsed(), online); controller.pump(clock.elapsed(), online); return condition(); }, timeout);
    };
    ASSERT_TRUE(wait([&] { return panel.coordinator().controller().capability_version() == 2; }));
    ASSERT_TRUE(panel.coordinator().invoke("terminal.open", {}, "open-shell").value("ok").toBool());
    ASSERT_TRUE(wait([&] { return panel.coordinator().controller().prompt_ready(); }))
        << QJsonDocument(panel.coordinator().invoke("terminal.read", {})).toJson().toStdString();
    auto& terminal = panel.coordinator();
    EXPECT_EQ(terminal.invoke("operation.status", {}, "open-shell").value("state"), "succeeded");
    const auto execute = [&](const QString& id, const QString& command) {
        const auto accepted = terminal.invoke("terminal.exec", {{"command", command}}, id);
        EXPECT_TRUE(accepted.value("ok").toBool()) << QJsonDocument(accepted).toJson().toStdString();
        const auto complete = wait([&] { const auto value = terminal.invoke("operation.status", {}, id).value("state").toString();
            return value != "running" && value != "submitted"; }, id == "flood" ? 30000 : 15000);
        if (!complete) ADD_FAILURE() << QJsonDocument(terminal.invoke("operation.status", {}, id)).toJson().toStdString()
            << QJsonDocument(terminal.invoke("terminal.status", {})).toJson().toStdString();
        return complete;
    };
    ASSERT_TRUE(execute("first", "$sharedWorkspaceValue=41; Write-Output ('VALUE='+$sharedWorkspaceValue)"));
    auto result = terminal.invoke("operation.result", {}, "first");
    EXPECT_EQ(result.value("state"), "succeeded"); EXPECT_TRUE(result.value("powershell_success").toBool());
    EXPECT_TRUE(result.value("output").toString().contains("VALUE=41")) << QJsonDocument(result).toJson().toStdString();
    ASSERT_TRUE(execute("second", "$sharedWorkspaceValue++; Write-Output ('VALUE='+$sharedWorkspaceValue)"));
    EXPECT_TRUE(terminal.invoke("operation.result", {}, "second").value("output").toString().contains("VALUE=42"))
        << QJsonDocument(terminal.invoke("operation.result", {}, "second")).toJson().toStdString();
    ASSERT_TRUE(terminal.invoke("terminal.input", {{"text", "Write-Output 'not-submitted'"}}).value("ok").toBool());
    EXPECT_EQ(terminal.invoke("terminal.exec", {{"command", "42"}}, "dirty").value("error"), "terminal_busy");
    ASSERT_TRUE(terminal.invoke("terminal.input", {{"text", "\x03"}}).value("ok").toBool());
    ASSERT_TRUE(wait([&] { return terminal.controller().prompt_ready(); }));
    ASSERT_TRUE(execute("failure", "Write-Error 'expected-workspace-test-error'"));
    EXPECT_EQ(terminal.invoke("operation.status", {}, "failure").value("powershell_success"), false);
    ASSERT_TRUE(execute("native", "cmd /c exit 7"));
    EXPECT_EQ(terminal.invoke("operation.status", {}, "native").value("last_native_exit_code"), 7);
    EXPECT_EQ(terminal.invoke("operation.status", {}, "native").value("powershell_success"), false);
    ASSERT_TRUE(execute("powershell", "Write-Output 'still-successful'"));
    EXPECT_EQ(terminal.invoke("operation.status", {}, "powershell").value("last_native_exit_code"), 7);
    EXPECT_EQ(terminal.invoke("operation.status", {}, "powershell").value("powershell_success"), true);
    terminal.set_blocked(true);
    EXPECT_EQ(terminal.invoke("terminal.open", {}, "blocked-open").value("error"), "terminal_transfer_busy");
    EXPECT_EQ(terminal.invoke("terminal.exec", {{"command", "42"}}, "blocked").value("error"), "terminal_not_ready");
    EXPECT_TRUE(terminal.invoke("terminal.read", {}).value("ok").toBool());
    terminal.set_blocked(false);
    ASSERT_TRUE(terminal.invoke("terminal.exec", {{"command", "Start-Sleep -Seconds 30"}}, "cancel").value("ok").toBool());
    ASSERT_TRUE(wait([&] { return terminal.invoke("operation.status", {}, "cancel").value("state") == "running"; }))
        << QJsonDocument(terminal.invoke("operation.status", {}, "cancel")).toJson().toStdString()
        << terminal.invoke("terminal.read", {}).value("output").toString().right(1500).toStdString();
    EXPECT_EQ(terminal.invoke("terminal.exec", {{"command", "42"}}, "busy").value("error"), "terminal_busy");
    ASSERT_TRUE(terminal.invoke("terminal.cancel", {}, "cancel").value("ok").toBool());
    ASSERT_TRUE(terminal.invoke("terminal.cancel", {}, "cancel").value("ok").toBool());
    ASSERT_TRUE(wait([&] { return terminal.controller().prompt_ready(); }))
        << QJsonDocument(terminal.invoke("terminal.status", {})).toJson().toStdString()
        << QJsonDocument(terminal.invoke("operation.status", {}, "cancel")).toJson().toStdString()
        << terminal.invoke("terminal.read", {}).value("output").toString().right(1800).toStdString();
    EXPECT_EQ(terminal.invoke("operation.status", {}, "cancel").value("state"), "cancelled");
    ASSERT_TRUE(execute("flood", "1..1030 | ForEach-Object { 'z'*1024 }"));
    EXPECT_TRUE(terminal.invoke("operation.result", {}, "flood").value("gap").toBool());
    EXPECT_TRUE(terminal.invoke("operation.result", {}, "first").value("gap").toBool());
    EXPECT_TRUE(terminal.invoke("operation.result", {}, "first").value("output").toString().isEmpty());
    EXPECT_LE(terminal.invoke("terminal.status", {}).value("buffer_bytes").toInt(), 1024*1024);
    ASSERT_TRUE(terminal.invoke("terminal.exec", {{"command", "Start-Sleep -Seconds 30"}}, "lost").value("ok").toBool());
    online = false; controller.channel_open(false); host.channel_open(false);
    ASSERT_TRUE(wait([&] { return terminal.invoke("operation.status", {}, "lost").value("state") == "unknown"; }));
    online = true; controller.channel_open(true); host.channel_open(true);
    ASSERT_TRUE(wait([&] { return terminal.controller().input_enabled(); }));
    EXPECT_EQ(terminal.invoke("operation.status", {}, "lost").value("state"), "unknown");
    EXPECT_TRUE(terminal.controller().execution_id().empty());
    ASSERT_TRUE(wait([&] { return terminal.controller().prompt_ready(); }));
    ASSERT_TRUE(execute("shell-exit", "[Environment]::Exit(17)"));
    EXPECT_EQ(terminal.invoke("operation.status", {}, "shell-exit").value("state"), "unknown");
    EXPECT_FALSE(panel.isVisible());
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv); testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS();
}
