#include <gtest/gtest.h>

#include <functional>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include "ui/agent_settings_dialog.h"

namespace {

class ScopedEnvironment final {
public:
    ScopedEnvironment(const char* name, const QByteArray& value)
        : name_(name), existed_(qEnvironmentVariableIsSet(name)), previous_(qgetenv(name)) {
        qputenv(name_, value);
    }

    ~ScopedEnvironment() {
        if (existed_) {
            qputenv(name_, previous_);
        } else {
            qunsetenv(name_);
        }
    }

private:
    const char* name_;
    bool existed_ = false;
    QByteArray previous_;
};

bool wait_until(const std::function<bool()>& condition, int timeout_ms = 5000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return condition();
}

bool write_file(const QString& path, const QByteArray& content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(content) == content.size();
}

bool write_provider_fixtures(const QString& directory) {
#ifdef _WIN32
    const QString source = QString::fromUtf8(REDCLAW_AGENT_ACCOUNT_FIXTURE_PATH);
    return QFile::copy(source, QDir(directory).filePath("codex.exe"))
        && QFile::copy(source, QDir(directory).filePath("cursor-agent.exe"));
#else
    Q_UNUSED(directory);
    return false;
#endif
}

QByteArray fixture_path(const QString& directory) {
    // Keep the real Codex/Cursor installation out of this test.
    return QDir::toNativeSeparators(directory).toLocal8Bit();
}

bool wait_for_initial_probe(redclaw::ui::AgentSettingsDialog* dialog) {
    auto* status = dialog->findChild<QLabel*>("agentAccountOperationStatus");
    auto* login = dialog->findChild<QPushButton*>("codexLoginButton");
    return status != nullptr && login != nullptr
        && wait_until([&] {
            return login->isEnabled()
                && (status->text().contains("No local Agent provider is ready")
                    || status->text().contains("At least one local Agent provider is ready"));
        }, 15000);
}

TEST(AgentSettingsDialog, ShowsLoginOutputAndNonZeroExit) {
#ifndef _WIN32
    GTEST_SKIP() << "Agent provider account commands are Windows-only";
#else
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(write_provider_fixtures(directory.path()));
    ScopedEnvironment path("PATH", fixture_path(directory.path()));
    ScopedEnvironment mode("REDCLAW_AGENT_LOGIN_TEST_MODE", "fail");
    QSettings settings(directory.filePath("settings.ini"), QSettings::IniFormat);
    redclaw::ui::AgentSettingsDialog dialog(&settings);
    dialog.show();
    ASSERT_TRUE(wait_for_initial_probe(&dialog));

    auto* login = dialog.findChild<QPushButton*>("codexLoginButton");
    auto* status = dialog.findChild<QLabel*>("agentAccountOperationStatus");
    auto* output = dialog.findChild<QPlainTextEdit*>("agentAccountOperationOutput");
    ASSERT_NE(login, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(output, nullptr);
    login->click();

    ASSERT_TRUE(wait_until([&] { return status->text().contains("exited with code 17"); }));
    EXPECT_TRUE(output->toPlainText().contains("Open browser to continue"));
    EXPECT_TRUE(output->toPlainText().contains("Browser launch unavailable"));
    EXPECT_TRUE(status->text().contains("codex login --device-auth"));
#endif
}

TEST(AgentSettingsDialog, ReportsLoginProcessStartFailure) {
#ifndef _WIN32
    GTEST_SKIP() << "Agent provider account commands are Windows-only";
#else
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(write_provider_fixtures(directory.path()));
    ASSERT_TRUE(write_file(QDir(directory.path()).filePath("codex.exe"), "not an executable"));
    ScopedEnvironment path("PATH", fixture_path(directory.path()));
    QSettings settings(directory.filePath("settings.ini"), QSettings::IniFormat);
    redclaw::ui::AgentSettingsDialog dialog(&settings);
    dialog.show();
    ASSERT_TRUE(wait_for_initial_probe(&dialog));

    auto* login = dialog.findChild<QPushButton*>("codexLoginButton");
    auto* status = dialog.findChild<QLabel*>("agentAccountOperationStatus");
    auto* output = dialog.findChild<QPlainTextEdit*>("agentAccountOperationOutput");
    ASSERT_NE(login, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(output, nullptr);
    login->click();

    ASSERT_TRUE(wait_until([&] { return status->text().contains("Failed to start Codex login"); }));
    EXPECT_FALSE(output->toPlainText().isEmpty());
    EXPECT_TRUE(status->text().contains("codex login --device-auth"));
#endif
}

TEST(AgentSettingsDialog, KeepsActiveLoginVisibleUntilCancelled) {
#ifndef _WIN32
    GTEST_SKIP() << "Agent provider account commands are Windows-only";
#else
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(write_provider_fixtures(directory.path()));
    ScopedEnvironment path("PATH", fixture_path(directory.path()));
    ScopedEnvironment mode("REDCLAW_AGENT_LOGIN_TEST_MODE", "wait");
    QSettings settings(directory.filePath("settings.ini"), QSettings::IniFormat);
    redclaw::ui::AgentSettingsDialog dialog(&settings);
    dialog.show();
    ASSERT_TRUE(wait_for_initial_probe(&dialog));

    auto* login = dialog.findChild<QPushButton*>("codexLoginButton");
    auto* cancel = dialog.findChild<QPushButton*>("agentAccountCancelButton");
    auto* status = dialog.findChild<QLabel*>("agentAccountOperationStatus");
    auto* output = dialog.findChild<QPlainTextEdit*>("agentAccountOperationOutput");
    ASSERT_NE(login, nullptr);
    ASSERT_NE(cancel, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(output, nullptr);
    login->click();
    ASSERT_TRUE(wait_until([&] {
        return output->toPlainText().contains("Waiting for browser completion");
    }));

    dialog.close();
    QCoreApplication::processEvents();
    EXPECT_TRUE(dialog.isVisible());
    EXPECT_TRUE(status->text().contains("Cancel account operation"));

    ASSERT_TRUE(cancel->isEnabled());
    cancel->click();
    ASSERT_TRUE(wait_until([&] { return status->text().contains("was cancelled"); }));
    EXPECT_FALSE(cancel->isEnabled());
#endif
}

}  // namespace
