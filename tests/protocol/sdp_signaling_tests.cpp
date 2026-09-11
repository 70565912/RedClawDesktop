#include "redclaw/protocol/sdp_signaling.h"

#include <gtest/gtest.h>

namespace {

TEST(SdpSignalingTest, ParsesStandardDescriptionAttributes) {
    const auto attributes = redclaw::protocol::parse_sdp_signaling_attributes(
        "v=0\r\n"
        "a=ice-ufrag:localUfrag\r\n"
        "a=ice-pwd:localPassword\r\n"
        "a=fingerprint:sha-256 AA:BB:CC\r\n");

    EXPECT_TRUE(attributes.complete());
    EXPECT_EQ(attributes.ice_ufrag, "localUfrag");
    EXPECT_EQ(attributes.ice_pwd, "localPassword");
    EXPECT_EQ(attributes.fingerprint, "sha-256 AA:BB:CC");
    EXPECT_TRUE(attributes.first_missing_reason().empty());
}

TEST(SdpSignalingTest, AcceptsWhitespaceAndAsciiCaseDifferences) {
    const auto attributes = redclaw::protocol::parse_sdp_signaling_attributes(
        "v=0\n"
        "  A=ICE-UFRAG: controllerUfrag  \n"
        "\tA=ICE-PWD: controllerPassword\t\n"
        " A=FINGERPRINT: SHA-256 DD:EE:FF \n");

    EXPECT_TRUE(attributes.complete());
    EXPECT_EQ(attributes.ice_ufrag, "controllerUfrag");
    EXPECT_EQ(attributes.ice_pwd, "controllerPassword");
    EXPECT_EQ(attributes.fingerprint, "SHA-256 DD:EE:FF");
}

TEST(SdpSignalingTest, ReportsFirstMissingOrEmptyAttributeWithoutLeakingValues) {
    auto attributes = redclaw::protocol::parse_sdp_signaling_attributes(
        "v=0\n"
        "a=ice-ufrag:   \n"
        "a=ice-pwd:password\n"
        "a=fingerprint:sha-256 AA:BB\n");
    EXPECT_FALSE(attributes.complete());
    EXPECT_EQ(attributes.first_missing_reason(), "missing_ice_ufrag");

    attributes = redclaw::protocol::parse_sdp_signaling_attributes(
        "v=0\na=ice-ufrag:ufrag\na=fingerprint:sha-256 AA:BB\n");
    EXPECT_EQ(attributes.first_missing_reason(), "missing_ice_pwd");

    attributes = redclaw::protocol::parse_sdp_signaling_attributes(
        "v=0\na=ice-ufrag:ufrag\na=ice-pwd:password\n");
    EXPECT_EQ(attributes.first_missing_reason(), "missing_fingerprint");
}

TEST(SdpSignalingTest, DoesNotAcceptAttributeNamesEmbeddedInOtherLines) {
    const auto attributes = redclaw::protocol::parse_sdp_signaling_attributes(
        "v=0\nx-a=ice-ufrag:not-valid\na=ice-pwd:password\n"
        "a=fingerprint:sha-256 AA:BB\n");

    EXPECT_FALSE(attributes.complete());
    EXPECT_EQ(attributes.first_missing_reason(), "missing_ice_ufrag");
}

}  // namespace
