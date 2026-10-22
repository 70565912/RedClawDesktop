#include <gtest/gtest.h>

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStringList>
#include <QUuid>
#include <QWidget>

#include "ui/host_update_restart_guard.h"
#include "redclaw/diag/diag_module.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace redclaw::ui {

TEST(HostUpdateRestartGuard, OnlyCancelableShutdownWhileConnectedIsBlocked) {
    EXPECT_FALSE(HostUpdateRestartGuard::should_block_shutdown(false, 0));
    EXPECT_TRUE(HostUpdateRestartGuard::should_block_shutdown(true, 0));
    EXPECT_TRUE(HostUpdateRestartGuard::should_block_shutdown(true, 1));
    EXPECT_FALSE(HostUpdateRestartGuard::should_block_shutdown(true, 0x40000000));
    EXPECT_FALSE(HostUpdateRestartGuard::should_block_shutdown(true, 0x80000000));
    EXPECT_FALSE(HostUpdateRestartGuard::should_block_shutdown(true, 0xc0000001));
}

#ifdef _WIN32
class UpdateRestartPolicyTest : public testing::Test {
protected:
    QString id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString base_ = "HKEY_CURRENT_USER\\Software\\RedClawDesktop\\Tests\\UpdateRestartGuard";
    QString root_ = base_ + '\\' + id_;
    QString policy_ = root_ + "\\policy";
    QString journal_ = root_ + "\\journal";

    void TearDown() override {
        QSettings base(base_, QSettings::NativeFormat);
        base.remove(id_);
        base.sync();
    }
};

TEST_F(UpdateRestartPolicyTest, RestoresOriginalAndAbsentValuesWithoutChangingUpdateInstallMode) {
    QSettings initial(policy_, QSettings::NativeFormat);
    initial.setValue("ActiveHoursStart", quint32(8));
    initial.setValue("ActiveHoursEnd", quint32(17));
    initial.setValue("AU/AUOptions", quint32(3));
    initial.setValue("AU/NoAutoRebootWithLoggedOnUsers", quint32(1));
    initial.sync();
    QString error;
    UpdateRestartPolicyLease lease(policy_, journal_);
    ASSERT_TRUE(lease.acquire(20, &error)) << error.toStdString();
    QSettings active(policy_, QSettings::NativeFormat);
    EXPECT_EQ(active.value("SetActiveHours").toUInt(), 1u);
    EXPECT_EQ(active.value("ActiveHoursStart").toUInt(), 20u);
    EXPECT_EQ(active.value("ActiveHoursEnd").toUInt(), 14u);
    EXPECT_EQ(active.value("AU/AUOptions").toUInt(), 3u);
    EXPECT_EQ(active.value("AU/NoAutoRebootWithLoggedOnUsers").toUInt(), 0u);
    ASSERT_TRUE(lease.release(&error)) << error.toStdString();
    QSettings restored(policy_, QSettings::NativeFormat);
    EXPECT_EQ(restored.value("ActiveHoursStart").toUInt(), 8u);
    EXPECT_EQ(restored.value("ActiveHoursEnd").toUInt(), 17u);
    EXPECT_EQ(restored.value("AU/NoAutoRebootWithLoggedOnUsers").toUInt(), 1u);
    EXPECT_EQ(restored.value("AU/AUOptions").toUInt(), 3u);
    EXPECT_FALSE(restored.contains("SetActiveHours"));
    EXPECT_FALSE(restored.contains("AU/AlwaysAutoRebootAtScheduledTime"));
    EXPECT_FALSE(QSettings(journal_, QSettings::NativeFormat).contains("lease"));
}

TEST_F(UpdateRestartPolicyTest, RefreshSpansMidnightAndDoesNotRewriteUnchangedJournal) {
    QString error;
    UpdateRestartPolicyLease lease(policy_, journal_);
    ASSERT_TRUE(lease.acquire(23, &error)) << error.toStdString();
    QSettings journal(journal_, QSettings::NativeFormat);
    const auto before = journal.value("lease").toByteArray();
    ASSERT_TRUE(lease.refresh(23, &error));
    journal.sync();
    EXPECT_EQ(journal.value("lease").toByteArray(), before);
    ASSERT_TRUE(lease.refresh(0, &error)) << error.toStdString();
    QSettings policy(policy_, QSettings::NativeFormat);
    EXPECT_EQ(policy.value("ActiveHoursStart").toUInt(), 0u);
    EXPECT_EQ(policy.value("ActiveHoursEnd").toUInt(), 18u);
    EXPECT_TRUE(lease.release(&error));
    EXPECT_TRUE(QSettings(policy_, QSettings::NativeFormat).allKeys().isEmpty());
}

TEST_F(UpdateRestartPolicyTest, ExternalPolicyChangeOrDeletionIsNotOverwritten) {
    QString error;
    UpdateRestartPolicyLease lease(policy_, journal_);
    ASSERT_TRUE(lease.acquire(10, &error)) << error.toStdString();
    QSettings external(policy_, QSettings::NativeFormat);
    external.setValue("ActiveHoursEnd", quint32(6));
    external.remove("SetActiveHours");
    external.sync();
    EXPECT_FALSE(lease.refresh(11, &error));
    EXPECT_EQ(error, "update_policy_changed_externally");
    ASSERT_TRUE(lease.release(&error));
    QSettings restored(policy_, QSettings::NativeFormat);
    EXPECT_EQ(restored.value("ActiveHoursEnd").toUInt(), 6u);
    EXPECT_FALSE(restored.contains("SetActiveHours"));
    EXPECT_FALSE(restored.contains("ActiveHoursStart"));
}

TEST_F(UpdateRestartPolicyTest, InvalidHourAndNonnumericPolicyFailBeforeMutation) {
    QString error;
    UpdateRestartPolicyLease lease(policy_, journal_);
    EXPECT_FALSE(lease.acquire(24, &error));
    QSettings policy(policy_, QSettings::NativeFormat);
    policy.setValue("ActiveHoursStart", QString("8"));
    policy.sync();
    EXPECT_FALSE(lease.acquire(8, &error));
    EXPECT_EQ(error, "update_policy_value_invalid");
    EXPECT_EQ(policy.value("ActiveHoursStart").toString(), "8");
    EXPECT_FALSE(policy.contains("SetActiveHours"));
    EXPECT_FALSE(QSettings(journal_, QSettings::NativeFormat).contains("lease"));
}

TEST_F(UpdateRestartPolicyTest, ExternalZeroDoesNotMatchAnOriginallyAbsentValue) {
    QString error;
    UpdateRestartPolicyLease lease(policy_, journal_);
    ASSERT_TRUE(lease.acquire(10, &error)) << error.toStdString();
    QSettings external(policy_, QSettings::NativeFormat);
    external.setValue("SetActiveHours", quint32(0));
    external.sync();
    ASSERT_TRUE(lease.release(&error));
    QSettings restored(policy_, QSettings::NativeFormat);
    EXPECT_TRUE(restored.contains("SetActiveHours"));
    EXPECT_EQ(restored.value("SetActiveHours").toUInt(), 0u);
}

TEST_F(UpdateRestartPolicyTest, RecoversPersistedLeaseAfterInterruptedOwner) {
    QString error;
    QByteArray interrupted;
    {
        QSettings policy(policy_, QSettings::NativeFormat);
        policy.setValue("ActiveHoursStart", quint32(8));
        policy.sync();
        UpdateRestartPolicyLease lease(policy_, journal_);
        ASSERT_TRUE(lease.acquire(21, &error)) << error.toStdString();
        interrupted = QSettings(journal_, QSettings::NativeFormat).value("lease").toByteArray();
    }
    // Recreate the state of a dead process without killing an application or
    // touching HKLM: its journal is durable and policy is partially applied.
    QSettings journal(journal_, QSettings::NativeFormat);
    journal.setValue("lease", interrupted);
    journal.sync();
    QSettings policy(policy_, QSettings::NativeFormat);
    policy.setValue("ActiveHoursStart", quint32(21));
    policy.setValue("SetActiveHours", quint32(1));
    policy.sync();
    UpdateRestartPolicyLease next(policy_, journal_);
    ASSERT_TRUE(next.recover(&error)) << error.toStdString();
    QSettings restored(policy_, QSettings::NativeFormat);
    EXPECT_EQ(restored.value("ActiveHoursStart").toUInt(), 8u);
    EXPECT_FALSE(restored.contains("SetActiveHours"));
    EXPECT_FALSE(QSettings(journal_, QSettings::NativeFormat).contains("lease"));
}

TEST_F(UpdateRestartPolicyTest, UnknownJournalVersionFailsClosed) {
    QSettings journal(journal_, QSettings::NativeFormat);
    journal.setValue("lease", QByteArray("{\"schema\":\"redclaw.update-restart-lease.v2\",\"version\":2,\"values\":[]}"));
    journal.sync();
    QString error;
    UpdateRestartPolicyLease lease(policy_, journal_);
    EXPECT_FALSE(lease.acquire(8, &error));
    EXPECT_EQ(error, "policy_journal_invalid");
    EXPECT_TRUE(QSettings(policy_, QSettings::NativeFormat).allKeys().isEmpty());
    EXPECT_TRUE(QSettings(journal_, QSettings::NativeFormat).contains("lease"));
}

TEST_F(UpdateRestartPolicyTest, ConcurrentOwnerCannotRecoverLiveLease) {
    QString error;
    UpdateRestartPolicyLease first(policy_, journal_);
    UpdateRestartPolicyLease second(policy_, journal_);
    ASSERT_TRUE(first.acquire(8, &error)) << error.toStdString();
    EXPECT_FALSE(second.acquire(19, &error));
    EXPECT_EQ(error, "another_host_owns_update_protection");
    EXPECT_EQ(QSettings(policy_, QSettings::NativeFormat).value("ActiveHoursStart").toUInt(), 8u);
    ASSERT_TRUE(first.release(&error));
    ASSERT_TRUE(second.acquire(19, &error)) << error.toStdString();
    EXPECT_EQ(QSettings(policy_, QSettings::NativeFormat).value("ActiveHoursStart").toUInt(), 19u);
}

TEST_F(UpdateRestartPolicyTest, ExistingDeadlineIsReportedAndNeverModified) {
    QSettings policy(policy_, QSettings::NativeFormat);
    policy.setValue("SetComplianceDeadline", quint32(1));
    policy.setValue("ConfigureDeadlineForQualityUpdates", quint32(0));
    policy.sync();
    QString error;
    UpdateRestartPolicyLease lease(policy_, journal_);
    EXPECT_TRUE(lease.deadline_configured());
    ASSERT_TRUE(lease.acquire(8, &error)) << error.toStdString();
    ASSERT_TRUE(lease.release(&error));
    policy.sync();
    EXPECT_EQ(policy.value("SetComplianceDeadline").toUInt(), 1u);
    EXPECT_EQ(policy.value("ConfigureDeadlineForQualityUpdates").toUInt(), 0u);
}

TEST_F(UpdateRestartPolicyTest, ExistingActiveHoursMaximumIsRespected) {
    QSettings policy(policy_, QSettings::NativeFormat);
    policy.setValue("SetActiveHoursMaxRange", quint32(1));
    policy.setValue("ActiveHoursMaxRange", quint32(8));
    policy.sync();
    QString error;
    UpdateRestartPolicyLease lease(policy_, journal_);
    ASSERT_TRUE(lease.acquire(20, &error)) << error.toStdString();
    policy.sync();
    EXPECT_EQ(policy.value("ActiveHoursEnd").toUInt(), 4u);
    ASSERT_TRUE(lease.release(&error));
    policy.sync();
    EXPECT_EQ(policy.value("ActiveHoursMaxRange").toUInt(), 8u);
    EXPECT_EQ(policy.value("SetActiveHoursMaxRange").toUInt(), 1u);
}

TEST_F(UpdateRestartPolicyTest, ProductionRuntimeStatusRequiresChannelsAndReleasesOnReconnect) {
    QWidget window;
    HostUpdateRestartGuard guard(window, {}, std::make_unique<UpdateRestartPolicyLease>(policy_, journal_));
    diag::DebugRuntimeStatus status;
    const auto consume = [&](const std::string& line) {
        EXPECT_TRUE(diag::update_debug_runtime_status_from_line(line, &status));
        guard.observe_session(status.role == "host", true, status.connected, status.channel_open);
    };
    MSG query{};
    query.hwnd = reinterpret_cast<HWND>(window.winId());
    query.message = WM_QUERYENDSESSION;
    qintptr result = TRUE;
    consume("Runtime state role=host state=3 remote_description_applied=true connected=true");
    EXPECT_FALSE(guard.nativeEventFilter("windows_generic_MSG", &query, &result));
    consume("Runtime desktop stream stats role=host channel_open=true");
    EXPECT_TRUE(guard.nativeEventFilter("windows_generic_MSG", &query, &result));
    consume("Runtime rebuilding signaling session role=host attempt=1");
    EXPECT_FALSE(guard.nativeEventFilter("windows_generic_MSG", &query, &result));
    EXPECT_FALSE(QSettings(journal_, QSettings::NativeFormat).contains("lease"));
}

TEST_F(UpdateRestartPolicyTest, NativeQueryIsVetoedOnlyForThisConnectedHostAndRestoredOnDisconnect) {
    QWidget window;
    QStringList messages;
    HostUpdateRestartGuard guard(window, [&](const QString& line) { messages.append(line); },
        std::make_unique<UpdateRestartPolicyLease>(policy_, journal_));
    MSG query{};
    query.hwnd = reinterpret_cast<HWND>(window.winId());
    query.message = WM_QUERYENDSESSION;
    qintptr result = TRUE;
    guard.observe_session(false, true, true, true);
    EXPECT_FALSE(guard.nativeEventFilter("windows_generic_MSG", &query, &result));
    guard.observe_session(true, true, true, false);
    EXPECT_FALSE(guard.nativeEventFilter("windows_generic_MSG", &query, &result));
    guard.observe_session(true, true, true, true);
    EXPECT_TRUE(guard.nativeEventFilter("windows_generic_MSG", &query, &result));
    EXPECT_EQ(result, FALSE);
    if (QGuiApplication::platformName() == "windows") {
        DWORD size = 0;
        ShutdownBlockReasonQuery(query.hwnd, nullptr, &size);
        EXPECT_GT(size, 0u);
        DWORD_PTR native_result = TRUE;
        ASSERT_NE(SendMessageTimeoutW(query.hwnd, WM_QUERYENDSESSION, 0, 0,
                                     SMTO_ABORTIFHUNG, 1000, &native_result), 0);
        EXPECT_EQ(native_result, FALSE);
    }
    query.lParam = ENDSESSION_CRITICAL;
    EXPECT_FALSE(guard.nativeEventFilter("windows_generic_MSG", &query, &result));
    query.lParam = 0;
    guard.observe_session(true, false, true, true);
    EXPECT_FALSE(guard.nativeEventFilter("windows_generic_MSG", &query, &result));
    EXPECT_FALSE(QSettings(journal_, QSettings::NativeFormat).contains("lease"));
    EXPECT_TRUE(messages.join('\n').contains("released"));
}
#endif
}  // namespace redclaw::ui
