#include "redclaw/net/ice_candidate_diagnostics.h"
#include <gtest/gtest.h>
#include <string>

namespace {
using redclaw::net::IceCandidateDiagnostics;
using redclaw::net::is_valid_candidate_diagnostic_reason;
const std::string sdp = "v=0\r\na=ice-pwd:abcdefghijklmnopqrstuv012345\r\n";
const std::string candidate = "candidate:1 1 udp 123 192.0.2.1 45678 typ host";

TEST(IceCandidateDiagnostics, OwnerAndReceiverAgreeWithoutLeakingTupleOrCredentials) {
    IceCandidateDiagnostics owner, receiver;
    owner.set_description(true, sdp); receiver.set_description(false, sdp);
    const auto local = owner.observe(true, candidate, "0", 100);
    const auto remote = receiver.observe(false, "a=candidate:99 1 UDP 999 192.0.2.1 45678 typ host", "0", 800);
    ASSERT_EQ(local.size(), 1U); ASSERT_EQ(remote.size(), 1U);
    EXPECT_EQ(local[0].identity, remote[0].identity);
    EXPECT_NE(local[0].first_seen_steady_ms, remote[0].first_seen_steady_ms);
    const auto reason = local[0].diagnostic_reason();
    EXPECT_TRUE(is_valid_candidate_diagnostic_reason(reason));
    EXPECT_EQ(reason.find("192.0.2.1"), std::string::npos);
    EXPECT_EQ(reason.find("45678"), std::string::npos);
    EXPECT_EQ(reason.find("abcdefghijklmnopqrstuv"), std::string::npos);
    EXPECT_TRUE(owner.observe(true, candidate, "0", 101).empty());
}
TEST(IceCandidateDiagnostics, CredentialsAndEndpointsSeparateGenerations) {
    IceCandidateDiagnostics a, b;
    a.set_description(true, sdp);
    b.set_description(true, "a=ice-pwd:012345abcdefghijklmnopqrstuv\r\n");
    const auto first = a.observe(true, candidate, "0", 1);
    ASSERT_EQ(first.size(), 1U);
    EXPECT_NE(first[0].identity, b.observe(true, candidate, "0", 1)[0].identity);
    EXPECT_NE(first[0].identity, a.observe(true, "candidate:1 1 udp 1 192.0.2.1 45679 typ host", "0", 2)[0].identity);
    a.reset();
    EXPECT_TRUE(a.observe(true, candidate, "0", 99).empty());
    const auto flushed = a.set_description(true, sdp);
    ASSERT_EQ(flushed.size(), 1U);
    EXPECT_EQ(flushed[0].first_seen_steady_ms, 99U);
}
TEST(IceCandidateDiagnostics, LateDescriptionFloodAndInvalidInputAreBounded) {
    IceCandidateDiagnostics tracker;
    for (unsigned i=0; i<1000; ++i)
        tracker.observe(true, "candidate:1 1 udp 1 192.0.2.1 " + std::to_string(10000+i) + " typ host", "0", i);
    EXPECT_EQ(tracker.set_description(true, sdp).size(), IceCandidateDiagnostics::kCapacity);
    EXPECT_TRUE(tracker.observe(true, candidate, "0", 1001).empty());
    tracker.reset(); tracker.set_description(false, sdp);
    EXPECT_TRUE(tracker.observe(false, "candidate:1 1 udp 1 192.0.2.1 65536 typ host", "0", 1).empty());
    EXPECT_TRUE(tracker.observe(false, candidate + "\nsecret", "0", 1).empty());
    EXPECT_TRUE(tracker.observe(false, std::string(4097, 'x'), "0", 1).empty());
    EXPECT_TRUE(tracker.observe(false, candidate, "0|injection", 1).empty());
    EXPECT_FALSE(is_valid_candidate_diagnostic_reason("candidate_local:192.0.2.1:v4:host:1"));
    EXPECT_FALSE(is_valid_candidate_diagnostic_reason("candidate_local:0123456789abcdef0123456789abcdef:v4:host:1:extra"));
}
TEST(IceCandidateDiagnostics, MissingAmbiguousCredentialsAndIpv6Canonicalization) {
    IceCandidateDiagnostics a, b;
    EXPECT_TRUE(a.set_description(true, "a=ice-pwd:short\r\n").empty());
    EXPECT_TRUE(a.observe(true, candidate, "0", 1).empty());
    EXPECT_TRUE(a.set_description(true, sdp + "a=ice-pwd:012345abcdefghijklmnopqrstuv\r\n").empty());
    a.reset(); a.set_description(true, sdp); b.set_description(false, sdp);
    const auto x=a.observe(true, "candidate:1 1 udp 1 2001:db8::1 20000 typ srflx", "0", 1);
    const auto y=b.observe(false, "candidate:2 1 udp 2 2001:db8:0:0:0:0:0:1 20000 typ srflx", "0", 2);
    ASSERT_EQ(x.size(), 1U); ASSERT_EQ(y.size(), 1U);
    EXPECT_EQ(x[0].identity, y[0].identity);
}
} // namespace
