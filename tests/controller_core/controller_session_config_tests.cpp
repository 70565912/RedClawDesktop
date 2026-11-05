#include <gtest/gtest.h>

#include "redclaw/controller_core/controller_session.h"

namespace {

using redclaw::controller_core::ControllerSession;
using redclaw::controller_core::ControllerSessionConfig;
using redclaw::controller_core::ControllerSessionPhase;
using redclaw::controller_core::IControllerSessionObserver;

ControllerSessionConfig valid_config() {
    ControllerSessionConfig config;
    config.machine_code = "RC7TST01";
    config.password = "correct horse";
    config.mailbox_directory = "controller-core-test-mailbox";
    return config;
}

class NullObserver final : public IControllerSessionObserver {
public:
    void on_phase(ControllerSessionPhase, std::string_view) override { ++phase_events; }
    void on_encoded_frame(redclaw::net::ReassembledEncodedVideoFrame&&) override {}
    int phase_events = 0;
};

TEST(ControllerSessionConfig, AcceptsDesktopMachineCodeFormat) {
    std::string error;
    EXPECT_TRUE(redclaw::controller_core::validate_controller_session_config(valid_config(), &error)) << error;
}

TEST(ControllerSessionConfig, RejectsMachineCodeOutsideDesktopContract) {
    for (const char* code : {"", "rc7tst01", "RC7TST0", "RC7TST012", "RC7-ST01"}) {
        auto config = valid_config();
        config.machine_code = code;
        std::string error;
        EXPECT_FALSE(redclaw::controller_core::validate_controller_session_config(config, &error)) << code;
        EXPECT_FALSE(error.empty());
    }
}

TEST(ControllerSessionConfig, RejectsMissingSecretPortAndMailbox) {
    auto no_password = valid_config();
    no_password.password.clear();
    EXPECT_FALSE(redclaw::controller_core::validate_controller_session_config(no_password));

    auto no_port = valid_config();
    no_port.ice_udp_port = 0;
    EXPECT_FALSE(redclaw::controller_core::validate_controller_session_config(no_port));

    auto no_mailbox = valid_config();
    no_mailbox.mailbox_directory.clear();
    EXPECT_FALSE(redclaw::controller_core::validate_controller_session_config(no_mailbox));
}

TEST(ControllerSession, InvalidConfigFailsClosedWithoutTransport) {
    NullObserver observer;
    ControllerSession session(observer);
    auto config = valid_config();
    config.password.clear();
    std::string error;
    EXPECT_FALSE(session.start(std::move(config), &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(session.phase(), ControllerSessionPhase::kIdle);
    EXPECT_EQ(observer.phase_events, 0);
    EXPECT_FALSE(session.request_keyframe("test"));
}

TEST(ControllerSession, ViewportOutsideStreamControlLimitsIsRejected) {
    NullObserver observer;
    ControllerSession session(observer);
    EXPECT_FALSE(session.set_viewport(63, 720));
    EXPECT_FALSE(session.set_viewport(1280, 16385));
    EXPECT_TRUE(session.set_viewport(1280, 720));
}

}  // namespace
