#include "ui/runtime_maintenance_context.h"
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>

namespace {
TEST(RuntimeMaintenanceContext, PrivateImmutableSnapshotPreservesArgumentsAndRefusesUnprotectedCopies) {
#ifndef _WIN32
    GTEST_SKIP() << "Windows current-user maintenance context.";
#else
    redclaw::ui::RuntimeMaintenanceContext context;
    QString error;
    ASSERT_TRUE(context.prepare(&error)) << error.toStdString();
    struct Cleanup {
        QString file;
        ~Cleanup() {
            const QFileInfo info(file);
            const auto directory = info.absoluteDir();
            if (info.fileName() == "session.json" && QRegularExpression("^[0-9a-f-]{36}$").match(directory.dirName()).hasMatch()) {
                QFile::remove(file); QDir().rmdir(directory.absolutePath());
            }
        }
    } cleanup{context.path()};
    const QStringList gui{QCoreApplication::applicationFilePath(), "--gui-role", "host"};
    const QStringList runtime{"--cli", "--role", "host", "--session-code", "TESTONLY", "--agent-project-root", QString::fromUtf8("C:/测试项目/space folder")};
    ASSERT_TRUE(context.save(gui, runtime, QCoreApplication::applicationPid(), &error)) << error.toStdString();
    const auto snapshot = redclaw::ui::RuntimeMaintenanceContext::read(context.path(), &error);
    ASSERT_TRUE(snapshot) << error.toStdString();
    EXPECT_EQ(snapshot->gui_arguments, gui); EXPECT_EQ(snapshot->runtime_arguments, runtime);
    EXPECT_FALSE(context.save(gui, runtime, QCoreApplication::applicationPid(), &error));
    QTemporaryDir untrusted;
    ASSERT_TRUE(untrusted.isValid());
    QFile source(context.path()); ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const auto copied_bytes = source.readAll(); source.close();
    QFile copy(untrusted.filePath("copied.json")); ASSERT_TRUE(copy.open(QIODevice::WriteOnly));
    ASSERT_EQ(copy.write(copied_bytes), copied_bytes.size()); copy.close();
    EXPECT_FALSE(redclaw::ui::RuntimeMaintenanceContext::read(untrusted.filePath("copied.json"), &error));
    EXPECT_EQ(error, "maintenance_context_not_private_or_invalid");
    QFile file(context.path()); ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    auto record = QJsonDocument::fromJson(file.readAll()).object(); file.close();
    record.insert("role", "controller");
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(record).toJson()); file.close();
    EXPECT_FALSE(redclaw::ui::RuntimeMaintenanceContext::read(context.path(), &error));
    EXPECT_EQ(error, "maintenance_context_invalid");
#endif
}
}
