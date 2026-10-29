#include "ui/gui_startup_options.h"
#include <gtest/gtest.h>

namespace redclaw::ui {
namespace {
QStringList saved_host_arguments() {
    return {"redclaw_desktop", "--gui-auto-start", "--gui-role", "host",
            "--signal-transport", "dht", "--stream-smoke", "--enable-debug-control",
            "--allow-remote-agent", "--agent-project-manifest", "private-projects.json"};
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
} // namespace
} // namespace redclaw::ui
