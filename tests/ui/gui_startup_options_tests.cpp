#include "ui/gui_startup_options.h"
#include "ui/private_gui_profile.h"
#include "ui/playback_control_hint_overlay.h"
#include "ui/file_preview_session.h"
#include "runtime/desktop_preview_output.h"
#include "ui/playback/playback_backends.h"
#include "ui/playback/playback_frame_progress.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMainWindow>
#include <QSettings>
#include <QUuid>
#if defined(_WIN32) && defined(_DEBUG)
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QLoggingCategory>
#include <QProcess>
#include <QStandardPaths>
#include <qt_windows.h>
#include <type_traits>
#endif
#if defined(REDCLAW_ENABLE_QT_OPENGL)
#include <QtOpenGLWidgets/QOpenGLWidget>
#endif
#include <gtest/gtest.h>

#if defined(_WIN32) && defined(_DEBUG)
namespace {
bool above_sibling(HWND first, HWND second) {
    for (auto below = GetWindow(first, GW_HWNDNEXT); below; below = GetWindow(below, GW_HWNDNEXT))
        if (below == second) return true;
    return false;
}

class FirstShowObserver final : public QAbstractNativeEventFilter {
public:
    FirstShowObserver(HWND presenter, HWND marker) : presenter_(presenter), marker_(marker) {}

    bool nativeEventFilter(const QByteArray&, void* message, qintptr*) override {
        const auto* msg = static_cast<const MSG*>(message);
        if (msg->hwnd != presenter_) return false;
        if (msg->message == WM_ACTIVATE && LOWORD(msg->wParam) != WA_INACTIVE) activated = true;
        if (msg->message == WM_WINDOWPOSCHANGED && !saw_show &&
            (reinterpret_cast<const WINDOWPOS*>(msg->lParam)->flags & SWP_SHOWWINDOW)) {
            saw_show = true;
            behind_at_show = above_sibling(marker_, presenter_);
        }
        return false;
    }
    bool activated = false;
    bool saw_show = false;
    bool behind_at_show = false;

private:
    HWND presenter_;
    HWND marker_;
};
}

int run_private_startup_fixture(int argc, char** argv) {
    const std::string mode(argv[2]);
    if (mode == "ordinary") QStandardPaths::setTestModeEnabled(true);
    else if (!redclaw::ui::isolate_private_qt_logging()) return 11;
    QApplication app(argc, argv);
    if (mode == "ordinary" || mode == "logging") {
        const bool sentinel = QLoggingCategory("redclaw.fixture.logging").isDebugEnabled();
        if (sentinel != (mode == "logging")) return 12;
        if (mode == "logging" && (qEnvironmentVariableIsSet("QT_LOGGING_CONF") || qEnvironmentVariableIsSet("QT_LOGGING_RULES") ||
            QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation).contains(qEnvironmentVariable("REDCLAW_FIXTURE_ORDINARY_CONFIG"), Qt::CaseInsensitive))) return 13;
        return 0;
    }
    if (QGuiApplication::platformName() != "windows") return 14;
    if (mode == "reject") return redclaw::ui::create_gui_window(true) ? 15 : 0;
    // The parent puts this child on a separate, never switched-to desktop.
    wchar_t desktop_name[128]{};
    if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop_name,
            sizeof(desktop_name), nullptr) || !std::wstring_view(desktop_name).starts_with(L"RedClawGuiFixture-")) return 26;
    auto foreground_marker = std::make_unique<QWidget>(nullptr, Qt::Tool);
    QWidget lower_marker(nullptr, Qt::Tool);
    const auto marker = reinterpret_cast<HWND>(foreground_marker->winId());
    const auto lower = reinterpret_cast<HWND>(lower_marker.winId());
    constexpr UINT no_activate = SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    if (!SetWindowPos(marker, HWND_BOTTOM, 50, 50, 260, 180, SWP_SHOWWINDOW | no_activate) ||
        !SetWindowPos(lower, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | no_activate)) return 19;
    auto window = redclaw::ui::create_gui_window(true);
    if (!window || window->isVisible() || !window->testAttribute(Qt::WA_ShowWithoutActivating) ||
        window->windowType() != Qt::Tool || window->windowFlags().testFlag(Qt::WindowStaysOnBottomHint) ||
        !window->windowFlags().testFlag(Qt::WindowTransparentForInput) ||
        !window->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus)) return 16;
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    if (!IsWindow(hwnd) || IsWindowVisible(hwnd) || GetForegroundWindow() == hwnd || GetActiveWindow() == hwnd) return 17;
    if (!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE)) return 18;
    if (!above_sibling(hwnd, marker)) return 20;
    FirstShowObserver observer(hwnd, marker);
    app.installNativeEventFilter(&observer);
    window->setGeometry(60, 60, 160, 100);
    if (!redclaw::ui::show_private_gui_window_behind(*window, foreground_marker->winId())) return 21;
    RECT presenter_rect{}, marker_rect{}, overlap{};
    if (!IsWindowVisible(hwnd) || !IsWindowVisible(marker) || !GetWindowRect(hwnd, &presenter_rect) ||
        !GetWindowRect(marker, &marker_rect) || !IntersectRect(&overlap, &presenter_rect, &marker_rect) ||
        !observer.saw_show || !observer.behind_at_show || !above_sibling(marker, hwnd) ||
        !above_sibling(hwnd, lower)) return 22;
    if (!SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | no_activate) ||
        !above_sibling(marker, hwnd)) return 23;
    if (SendMessageW(hwnd, WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(hwnd),
            MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)) != MA_NOACTIVATEANDEAT) return 24;
    foreground_marker.reset();
    app.processEvents();
    if (!(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_NOACTIVATE) ||
        observer.activated || hwnd == GetForegroundWindow() || hwnd == GetActiveWindow()) return 25;
    return 0;
}
#endif

namespace redclaw::ui {
namespace {
using runtime::DesktopVideoMode;

TEST(GuiStartupOptions, PrivateProfileCannotBeReplayedOrShownOffscreen) {
    GuiAutoStartOptions gui;
    QString error;
    EXPECT_FALSE(parse_gui_auto_start_options({"app", "--gui-test-profile", "test-dir"}, &gui, &error,
        GuiStartupOrigin::kValidatedMaintenance));
    if (QGuiApplication::platformName() == "offscreen") EXPECT_FALSE(create_gui_window(true));
}

TEST(GuiStartupOptions, PrivateSettingsHaveNoFallbackAndPersistOnlyToTheirOwnIni) {
    QTemporaryDir a, b;
    ASSERT_TRUE(a.isValid() && b.isValid());
    GuiAutoStartOptions gui;
    gui.private_profile_dir = a.path();
    const QString key = "private-test/sentinel";
    const auto open = [&] { return std::unique_ptr<QSettings>(open_gui_settings(gui, {}, nullptr)); };
    {
        auto settings = open();
        EXPECT_EQ(settings->format(), QSettings::IniFormat);
        EXPECT_FALSE(settings->fallbacksEnabled());
        EXPECT_EQ(settings->fileName(), a.filePath("ui.ini"));
        settings->setValue(key, "first");
        settings->sync();
        ASSERT_EQ(settings->status(), QSettings::NoError);
    }
    gui.private_profile_dir = b.path();
    EXPECT_FALSE(open()->contains(key));
    open()->setValue(key, "second");
    gui.private_profile_dir = a.path();
    EXPECT_EQ(open()->value(key).toString(), "first");
    EXPECT_EQ(QSettings(b.filePath("ui.ini"), QSettings::IniFormat).value(key).toString(), "second");
}

#if defined(_WIN32) && defined(_DEBUG)
TEST(GuiStartupOptions, OnlyPrivatePresenterIgnoresCoveringWindows) {
    const QRect canvas(10, 20, 320, 180);
    QRect opaque_cover = canvas;
    int scans = 0;
    const auto covered = [&](const QRect& bounds) {
        ++scans;
        return opaque_cover.contains(bounds);
    };
    EXPECT_TRUE(playback_bounds_occluded(canvas, PlaybackPresenter::kOrdinary, covered));
    EXPECT_FALSE(playback_bounds_occluded(canvas, PlaybackPresenter::kPrivateTest, covered));
    opaque_cover.setWidth(160);
    EXPECT_FALSE(playback_bounds_occluded(canvas, PlaybackPresenter::kOrdinary, covered));
    EXPECT_TRUE(playback_bounds_occluded({}, PlaybackPresenter::kOrdinary, covered));
    EXPECT_TRUE(playback_bounds_occluded({}, PlaybackPresenter::kPrivateTest, covered));
    EXPECT_EQ(scans, 2);
}

TEST(GuiStartupOptions, PrivateHintsStayHiddenOnReconnectAndControlFeedback) {
    QWidget canvas;
    PlaybackControlHintOverlay hint(&canvas, false);
    canvas.show();
    hint.show_hint("reconnecting", 0);
    hint.show_hint("control");
    hint.show_clipboard_failure("clipboard");
    EXPECT_TRUE(hint.isHidden());
    EXPECT_FALSE(hint.internalWinId());
    EXPECT_TRUE(hint.message().isEmpty());
}

TEST(GuiStartupOptions, PrivateBootstrapIsolatesLoggingAndShowsBelowOverlappingSibling) {
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    QFile config(root.filePath("sentinel.ini"));
    ASSERT_TRUE(config.open(QIODevice::WriteOnly));
    const QByteArray rules("[Rules]\nredclaw.fixture.logging.debug=false\n");
    ASSERT_EQ(config.write(rules), rules.size());
    config.close();
    auto desktop_name = ("RedClawGuiFixture-" + QUuid::createUuid().toString(QUuid::WithoutBraces)).toStdWString();
    const std::unique_ptr<std::remove_pointer_t<HDESK>, decltype(&CloseDesktop)> desktop(
        CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr), &CloseDesktop);
    ASSERT_TRUE(desktop) << GetLastError();
    for (auto [mode, command] : {std::pair{"ordinary", SW_SHOWNA}, {"logging", SW_SHOWNA},
            {"create", SW_SHOWNOACTIVATE}, {"create", SW_SHOWNA}, {"reject", SW_SHOWNORMAL}}) {
        SCOPED_TRACE(std::string(mode) + ":" + std::to_string(command));
        QProcess child;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_QPA_PLATFORM", std::string_view(mode) == "create" || std::string_view(mode) == "reject" ? "windows" : "offscreen");
        env.insert("QT_LOGGING_CONF", config.fileName());
        env.insert("QT_LOGGING_DEBUG", "1");
        env.insert("QT_FORCE_STDERR_LOGGING", "1");
        env.insert("QT_LOGGING_RULES", "redclaw.fixture.unrelated=false");
        env.insert("REDCLAW_FIXTURE_ORDINARY_CONFIG", QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
        child.setProcessEnvironment(env);
        child.setCreateProcessArgumentsModifier([command, mode, &desktop_name](QProcess::CreateProcessArguments* args) {
            if (std::string_view(mode) == "create" || std::string_view(mode) == "reject")
                args->startupInfo->lpDesktop = desktop_name.data();
            args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
            args->startupInfo->wShowWindow = static_cast<WORD>(command);
        });
        child.start(QCoreApplication::applicationFilePath(), {"--private-startup-fixture", mode});
        ASSERT_TRUE(child.waitForFinished(15000)) << child.errorString().toStdString();
        ASSERT_EQ(child.exitStatus(), QProcess::NormalExit);
        const auto diagnostic = child.readAllStandardError();
        EXPECT_EQ(child.exitCode(), 0) << diagnostic.toStdString();
        EXPECT_EQ(diagnostic.contains("sentinel.ini"), std::string_view(mode) == "ordinary") << diagnostic.toStdString();
    }
}

TEST(GuiStartupOptions, PrivateProfileExclusivelyClaimsFreshOwnerOnlyRoutes) {
    const QDir parent(qEnvironmentVariable("LOCALAPPDATA"));
    const QString leaf = "RedClawGuiTest-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto root = parent.filePath(leaf);
    runtime::RuntimeOptions args;
    args.gui_test_profile = QDir::toNativeSeparators(root).toLocal8Bit().toStdString();
    args.ice_udp_port = 49176;
    GuiAutoStartOptions gui;
    ASSERT_TRUE(prepare_private_gui_profile(args, gui));
    struct Cleanup { QString path; ~Cleanup() { EXPECT_TRUE(QDir().rmdir(path)); } } cleanup{root};
    EXPECT_EQ(gui.role, "controller");
    EXPECT_TRUE(private_gui_worker_arguments(gui).contains("--no-system-clipboard"));
    EXPECT_TRUE(private_gui_worker_arguments({}).isEmpty());
    EXPECT_FALSE(gui.auto_start);
    EXPECT_FALSE(gui.video_override.has_value());
    EXPECT_EQ(gui.ice_udp_port, 49176);
    EXPECT_EQ(gui.signal_dir, QDir(root).filePath("signal"));
    EXPECT_EQ(gui.log_dir, QDir(root).filePath("logs"));
    EXPECT_EQ(gui.coordination_journal_path, QDir(root).filePath("coordination.jsonl"));
    EXPECT_TRUE(gui.enable_debug_control);
    EXPECT_FALSE(gui.enable_workspace_control || gui.enable_agent_control);
    EXPECT_EQ(gui.debug_control_name, leaf + ".debug");
    EXPECT_EQ(gui.workspace_control_name, leaf + ".workspace");
    EXPECT_EQ(gui.agent_control_name, leaf + ".agent");
    EXPECT_FALSE(prepare_private_gui_profile(args, gui));
    args.gui_test_profile = "../" + leaf.toStdString();
    EXPECT_FALSE(prepare_private_gui_profile(args, gui));
}
#endif

TEST(GuiStartupOptions, TimeLimitsRepresentExactSignedSecondsOrFail) {
    for (const auto& flag : {QString("--run-seconds"), QString("--signal-timeout-seconds")}) {
        GuiAutoStartOptions gui;
        QString error;
        EXPECT_FALSE(parse_gui_auto_start_options({"app", flag, "2147483648", flag, "0"}, &gui, &error));
        EXPECT_FALSE(parse_gui_auto_start_options({"app", flag, "0", flag, "2147483648"}, &gui, &error));
        EXPECT_FALSE(parse_gui_auto_start_options({"app", flag, "-1"}, &gui, &error));
        for (const auto& seconds : {QString("0"), QString("86401"), QString("2147483647")}) {
            ASSERT_TRUE(parse_gui_auto_start_options({"app", flag, seconds}, &gui, &error));
            EXPECT_EQ(QString::number(flag == "--run-seconds" ? gui.run_seconds : gui.signal_timeout_seconds), seconds);
            gui.private_profile_dir = "test-profile";
            const auto argv = private_gui_worker_arguments(gui);
            ASSERT_EQ(argv.count(flag), 1);
            EXPECT_EQ(argv.at(argv.indexOf(flag) + 1), seconds);
        }
    }
}

TEST(GuiStartupOptions, VideoSelectionDoesNotArmWaitingOrConnecting) {
    const QStringList base{"redclaw_desktop"};
    for (const QStringList& args : {base, base + QStringList{"--gui-role", "host", "--gui-persist-host-wait"},
            base + QStringList{"--gui-role", "controller"}, base + QStringList{"--stream-smoke"},
            base + QStringList{"--no-desktop-video"}}) {
        GuiAutoStartOptions options;
        QString error;
        ASSERT_TRUE(parse_gui_auto_start_options(args, &options, &error)) << error.toStdString();
        EXPECT_FALSE(options.auto_start);
    }
}

TEST(GuiStartupOptions, ExplicitStartsForwardVideoModeWithoutManufacturingSmoke) {
    for (const auto role : {"host", "controller"}) {
        for (const auto flag : {"", "--desktop-video", "--stream-smoke", "--stream-require-capture", "--no-desktop-video"}) {
            QStringList args{"redclaw_desktop", "--gui-auto-start", "--gui-role", role};
            if (*flag) args << flag;
            GuiAutoStartOptions options;
            QString error;
            ASSERT_TRUE(parse_gui_auto_start_options(args, &options, &error)) << error.toStdString();
            EXPECT_TRUE(options.auto_start);
            const auto mode = options.video_override.value_or(DesktopVideoMode::kDesktop);
            const auto worker = desktop_video_worker_arguments(mode);
            if (!*flag || QString(flag) == "--desktop-video") {
                EXPECT_EQ(mode, DesktopVideoMode::kDesktop);
                EXPECT_EQ(worker, QStringList{"--desktop-video"});
            } else {
                EXPECT_EQ(worker, QStringList{flag});
            }
        }
    }
}

TEST(GuiStartupOptions, DisabledVideoConflictsInEitherOrderAndStrictChecksWinOverSmoke) {
    for (const QString flag : {"--desktop-video", "--stream-smoke", "--stream-require-capture"}) {
        for (const QStringList& args : {QStringList{"app", flag, "--no-desktop-video"},
                QStringList{"app", "--no-desktop-video", flag}}) {
            GuiAutoStartOptions options;
            QString error;
            EXPECT_FALSE(parse_gui_auto_start_options(args, &options, &error));
            EXPECT_FALSE(error.isEmpty());
        }
    }
    for (const QStringList& args : {QStringList{"app", "--stream-require-capture", "--stream-smoke"},
            QStringList{"app", "--stream-smoke", "--stream-require-capture"}}) {
        GuiAutoStartOptions options;
        QString error;
        ASSERT_TRUE(parse_gui_auto_start_options(args, &options, &error));
        EXPECT_EQ(options.video_override, DesktopVideoMode::kRequireCapture);
    }
}

QStringList saved_host_arguments() {
    return {"redclaw_desktop", "--gui-auto-start", "--gui-role", "host",
            "--signal-transport", "dht", "--stream-smoke", "--enable-debug-control",
            "--allow-remote-agent", "--agent-project-manifest", "private-projects.json"};
}

TEST(GuiStartupOptions, ResumeRestoresTheWorkerVideoChoiceNotTheOriginalGuiForm) {
    const QStringList gui{"app", "--gui-role", "host", "--stream-smoke"};
    for (const auto expected : {DesktopVideoMode::kDisabled, DesktopVideoMode::kDesktop, DesktopVideoMode::kRequireCapture}) {
        const auto saved = gui + QStringList{"--cli"} + desktop_video_worker_arguments(expected);
        GuiAutoStartOptions options;
        QString error;
        ASSERT_TRUE(parse_gui_auto_start_options(saved, &options, &error, GuiStartupOrigin::kValidatedMaintenance, gui.size()));
        EXPECT_EQ(options.video_override.value_or(DesktopVideoMode::kDesktop), expected);
    }
}

TEST(GuiStartupOptions, RolelessStartupProfileFlowsIntoExplicitWorkerArguments) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    for (const auto& entry : {std::pair{"desktop_video=false", DesktopVideoMode::kDisabled},
            {"stream_require_capture=true", DesktopVideoMode::kRequireCapture}, {"stream_smoke=true", DesktopVideoMode::kSmoke}}) {
        QFile file(temp.filePath("video.conf"));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_GT(file.write(entry.first), 0);
        file.close();
        for (const auto& args : {QStringList{"app", "--runtime-config", file.fileName()},
                QStringList{"app", "--gui-role", "controller", "--runtime-config", file.fileName()},
                QStringList{"app", "--runtime-config", file.fileName(), "--gui-role", "host", "--gui-auto-start"}}) {
            GuiAutoStartOptions start;
            QString error;
            ASSERT_TRUE(parse_gui_auto_start_options(args, &start, &error)) << error.toStdString();
            EXPECT_EQ(start.auto_start, args.contains("--gui-auto-start"));
            ASSERT_EQ(start.video_override, entry.second);
            QStringList worker{"--cli", "--role", "host"};
            worker += desktop_video_worker_arguments(*start.video_override);
            ASSERT_TRUE(normalize_saved_video_arguments(worker, &error)) << error.toStdString();
            EXPECT_EQ(worker.last(), desktop_video_worker_arguments(entry.second).front());
        }
    }
}

TEST(GuiStartupOptions, FilePreviewRequiresSuccessfulOutputCurrentWorkerAndAuthenticatedGeneration) {
    using namespace redclaw::runtime;
    FilePreviewSession session;
    ASSERT_TRUE(session.start(true));
    DesktopPreviewOutput writer;
    const auto directory = std::filesystem::path(session.directory().toStdWString());
    const auto path = directory / "controller-preview.ppm";
    redclaw::render::DecodedVideoFrame pixel;
    pixel.width = pixel.height = 1; pixel.row_pitch = 4; pixel.pixels = {0, 0, 255, 255};
    std::string error;
    EXPECT_EQ(writer.publish(path, pixel, false, 1, 7, error), PreviewOutput::kSkipped);
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(writer.publish(directory / "absent" / "test.ppm", pixel, true, 1, 0, error), PreviewOutput::kFailed);
    EXPECT_FALSE(std::filesystem::exists(path));
    ASSERT_EQ(writer.publish(path, pixel, true, 1001, 7, error), PreviewOutput::kWritten) << error;
    EXPECT_EQ(writer.publish(path, pixel, true, 1002, 7, error), PreviewOutput::kSkipped);
    EXPECT_TRUE(session.read().isNull());
    session.apply({6, true});
    EXPECT_TRUE(session.read().isNull());
    session.apply({7, true});
    ASSERT_EQ(session.read().pixelColor(0, 0), QColor(Qt::red));
    EXPECT_FALSE(std::filesystem::exists(directory / "presented"));
    QWidget parent;
    const auto renderer = create_qt_playback_renderer(&parent);
    QString render_error;
    int draws = 0;
    ASSERT_EQ(session.present(*renderer.canvas, session.read(), &render_error, [&] { ++draws; }), PresentOutcome::kDeferred);
    EXPECT_EQ(draws, 0);
    EXPECT_FALSE(std::filesystem::exists(directory / "presented"));
    QImage surface(renderer.widget->size(), QImage::Format_RGB32);
    renderer.widget->render(&surface, QPoint(), QRegion(0, 0, 1, 1));
    EXPECT_FALSE(std::filesystem::exists(directory / "presented"));
    renderer.widget->render(&surface);
    EXPECT_EQ(surface.pixelColor(surface.rect().center()), QColor(Qt::red));
    EXPECT_EQ(draws, 1);
    EXPECT_TRUE(std::filesystem::exists(directory / "presented"));
    EXPECT_FALSE(controller_smoke_output(true, true, false));
    EXPECT_TRUE(controller_smoke_output(true, false, true));
    EXPECT_FALSE(controller_smoke_output(false, false, true));
    EXPECT_TRUE(controller_smoke_output(false, true, false));
    ASSERT_EQ(session.present(*renderer.canvas, session.read(), &render_error, [&] { ++draws; }), PresentOutcome::kDeferred);
    session.apply({7, false});
    EXPECT_TRUE(session.read().isNull());
    session.apply({7, true});
    EXPECT_TRUE(session.read().isNull());
    session.apply({8, true});
    EXPECT_TRUE(session.read().isNull());
    renderer.widget->render(&surface);
    EXPECT_EQ(draws, 1);
    ASSERT_TRUE(session.start(true));
    session.apply({7, true});
    EXPECT_TRUE(session.read().isNull());
    EXPECT_FALSE(std::filesystem::exists(directory));
}

TEST(GuiStartupOptions, DeferredFrameFeedbackBelongsOnlyToTheFrameActuallyDrawn) {
    QWidget parent;
    const auto renderer = create_qt_playback_renderer(&parent);
    DirectFrameData frame;
    frame.width = frame.height = 1; frame.row_pitch = 4; frame.pixels = {0, 0, 255, 255};
    PlaybackFrameProgress progress;
    protocol::StreamControlMessageV1 feedback;
    QString error;
    const auto submit = [&](quint64 id) {
        frame.frame_id = id;
        EXPECT_TRUE(progress.accept(progress.generation, id, true));
        const auto result = renderer.canvas->present_frame(frame, &error);
        EXPECT_EQ(result, PresentOutcome::kDeferred);
        renderer.canvas->after_draw(result, [&, id, generation = progress.generation] {
            progress.record_draw(generation, id);
        });
    };
    submit(1);
    submit(2);
    progress.stamp_feedback(feedback, 1, 2, 0);
    EXPECT_EQ(feedback.latest_displayable_frame_id, 2u);
    EXPECT_EQ(feedback.latest_presented_frame_id, 0u);
    EXPECT_EQ(feedback.rendered_frames, 0u);
    QImage surface(renderer.widget->size(), QImage::Format_RGB32);
    renderer.widget->render(&surface);
    progress.stamp_feedback(feedback, 1, 2, 1);
    EXPECT_EQ(feedback.latest_presented_frame_id, 2u);
    EXPECT_EQ(feedback.rendered_frames, 1u);
    submit(3);
    progress.reset_frames(progress.generation + 1);
    renderer.widget->render(&surface);
    EXPECT_EQ(progress.presented, 0u);
    submit(1);
    renderer.widget->render(&surface);
    EXPECT_EQ(progress.presented, 1u);
    submit(2);
    renderer.widget->render(&surface);
    EXPECT_EQ(progress.presented, 2u);
    submit(3);
    renderer.canvas->clear_frame();
    renderer.widget->render(&surface);
    EXPECT_EQ(progress.presented, 2u);
}

#if defined(REDCLAW_ENABLE_QT_OPENGL)
TEST(GuiStartupOptions, OpenGLFrameWithoutInitializedShaderCannotConfirmPresentation) {
    QWidget parent;
    const auto renderer = create_opengl_playback_renderer(&parent);
    DirectFrameData frame;
    frame.width = frame.height = 1; frame.row_pitch = 4; frame.pixels = {0, 0, 255, 255};
    QString error;
    const auto result = renderer.canvas->present_frame(frame, &error);
    EXPECT_EQ(result, PresentOutcome::kDeferred);
    int reports = 0;
    renderer.canvas->after_draw(result, [&] { ++reports; });
    struct PaintAccess : QOpenGLWidget { using QOpenGLWidget::paintGL; };
    (static_cast<QOpenGLWidget*>(renderer.widget)->*&PaintAccess::paintGL)();
    EXPECT_EQ(reports, 0);
}
#endif

TEST(GuiStartupOptions, ProfileMergeAndLegacyReplayRetainCaptureWithoutChangingUnspecifiedV1Default) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    QFile file(temp.filePath("host.conf"));
    QString error;
    for (const auto& entry : {std::pair{"role=host\n", "--no-desktop-video"},
            {"role=host\nstream_smoke=false", "--no-desktop-video"},
            {"role=host\nstream_smoke=true", "--stream-smoke"},
            {"role=host\nstream_require_capture=true", "--stream-require-capture"}}) {
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_GT(file.write(entry.first), 0); file.close();
        QStringList worker{"--cli", "--runtime-config", file.fileName()};
        ASSERT_TRUE(normalize_saved_video_arguments(worker, &error)) << error.toStdString();
        EXPECT_EQ(worker.last(), entry.second);
    }
    for (const auto& args : {QStringList{"app", "--no-desktop-video", "--runtime-config", file.fileName()},
            QStringList{"app", "--runtime-config", file.fileName(), "--no-desktop-video"}}) {
        GuiAutoStartOptions start;
        EXPECT_FALSE(parse_gui_auto_start_options(args, &start, &error));
    }
    std::optional<DesktopVideoMode> selected;
    ASSERT_TRUE(merge_gui_video_profile(file.fileName(), selected, &error));
    EXPECT_EQ(selected, DesktopVideoMode::kRequireCapture);
    selected = DesktopVideoMode::kDisabled;
    EXPECT_FALSE(merge_gui_video_profile(file.fileName(), selected, &error));
}

TEST(GuiStartupOptions, MaintenanceRestoresInteractiveAgentAuthorizationWithoutCliProjectRoots) {
    GuiAutoStartOptions options;
    QString error;
    ASSERT_TRUE(parse_gui_auto_start_options(saved_host_arguments(), &options, &error,
        GuiStartupOrigin::kValidatedMaintenance)) << error.toStdString();
    EXPECT_TRUE(options.auto_start);
    EXPECT_TRUE(options.allow_remote_agent);
    EXPECT_TRUE(options.agent_project_roots.isEmpty());
    EXPECT_EQ(options.role, "host");
    EXPECT_FALSE(options.input_diagnostics);
}

TEST(GuiStartupOptions, DiagnosticResumeUsesTheSameSavedProjectContract) {
    auto args = saved_host_arguments();
    args << "--input-diagnostics";
    GuiAutoStartOptions options;
    QString error;
#ifdef NDEBUG
    EXPECT_FALSE(parse_gui_auto_start_options(args, &options, &error,
        GuiStartupOrigin::kValidatedMaintenance));
    EXPECT_EQ(error, "--input-diagnostics is Debug-only");
#else
    ASSERT_TRUE(parse_gui_auto_start_options(args, &options, &error,
        GuiStartupOrigin::kValidatedMaintenance)) << error.toStdString();
    EXPECT_TRUE(options.allow_remote_agent);
    EXPECT_TRUE(options.input_diagnostics);
#endif
}

TEST(GuiStartupOptions, FreshAutomationStillRequiresExplicitProjectRoots) {
    GuiAutoStartOptions options;
    QString error;
    EXPECT_FALSE(parse_gui_auto_start_options(saved_host_arguments(), &options, &error));
    EXPECT_EQ(error, "--allow-remote-agent GUI automation requires --agent-project-root");
    auto args = saved_host_arguments();
    args << "--agent-project-root" << "fixture/project";
    options = {};
    EXPECT_TRUE(parse_gui_auto_start_options(args, &options, &error)) << error.toStdString();
    EXPECT_EQ(options.agent_project_roots, QStringList{"fixture/project"});
}

TEST(GuiStartupOptions, MaintenanceDoesNotBypassDiagnosticSafetyChecks) {
    auto args = saved_host_arguments();
    args.removeAll("--enable-debug-control");
    args << "--input-diagnostics";
    GuiAutoStartOptions options;
    QString error;
    EXPECT_FALSE(parse_gui_auto_start_options(args, &options, &error,
        GuiStartupOrigin::kValidatedMaintenance));
    EXPECT_FALSE(error.isEmpty());
}

TEST(GuiStartupOptions, VideoCodecPreferenceDefaultsToAutoAndAcceptsRollback) {
    GuiAutoStartOptions options;
    QString error;
    const QStringList base{"redclaw_desktop", "--gui-auto-start", "--gui-role", "controller"};
    ASSERT_TRUE(parse_gui_auto_start_options(base, &options, &error)) << error.toStdString();
    EXPECT_EQ(options.stream_video_codec, redclaw::protocol::VideoCodecPreference::kAuto);

    auto forced = base;
    forced << "--stream-video-codec" << "h264";
    options = {};
    ASSERT_TRUE(parse_gui_auto_start_options(forced, &options, &error)) << error.toStdString();
    EXPECT_EQ(options.stream_video_codec, redclaw::protocol::VideoCodecPreference::kH264);

    auto invalid = base;
    invalid << "--stream-video-codec" << "av1";
    options = {};
    EXPECT_FALSE(parse_gui_auto_start_options(invalid, &options, &error));
    EXPECT_TRUE(error.contains("--stream-video-codec"));
}
} // namespace
} // namespace redclaw::ui
