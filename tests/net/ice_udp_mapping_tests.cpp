#include "redclaw/net/ice_udp_mapping.h"

#include <gtest/gtest.h>
#include <rtc/candidate.hpp>
#include <rtc/global.hpp>

namespace {
using namespace redclaw::net;
const IceUdpPortMapping kMapping{"192.0.2.10", 55000, "198.51.100.20", 55001};
const std::string kHost = "candidate:1 1 UDP 2122260223 192.0.2.10 55000 typ host";

TEST(IceUdpMapping, PublishesAssignedExternalPortWithUnchangedSocketBase) {
    struct NativeRuntime {
        NativeRuntime() { rtc::Preload(); }
        ~NativeRuntime() { rtc::Cleanup().wait(); }
    } runtime; // Candidate::resolve requires the native Winsock initialization on Windows.
    const auto candidate = mapped_udp_candidate(kHost, kMapping);
    ASSERT_TRUE(candidate);
    EXPECT_NE(candidate->find("198.51.100.20 55001 typ srflx raddr 192.0.2.10 rport 55000"), std::string::npos);
    // Existing native ICE parsers accept this standard candidate without new capabilities.
    rtc::Candidate parsed(*candidate, "0");
    ASSERT_TRUE(parsed.resolve(rtc::Candidate::ResolveMode::Simple));
    EXPECT_EQ(parsed.type(), rtc::Candidate::Type::ServerReflexive);
    EXPECT_EQ(parsed.address(), kMapping.external_address);
    EXPECT_EQ(parsed.port(), 55001);
    EXPECT_EQ(parsed.priority() >> 24U, 100U);
    auto same_port = kMapping;
    same_port.external_port = 55000;
    ASSERT_TRUE(mapped_udp_candidate(kHost, same_port));
    EXPECT_NE(mapped_udp_candidate(kHost, same_port)->find("198.51.100.20 55000 typ srflx"), std::string::npos);
}

TEST(IceUdpMapping, OnlyMatchingHostSocketGetsAdditionalRoute) {
    for (const auto* candidate : {
        "candidate:1 1 UDP 1 192.0.2.11 55000 typ host",
        "candidate:1 1 UDP 1 192.0.2.10 55002 typ host",
        "candidate:1 1 TCP 1 192.0.2.10 55000 typ host tcptype passive",
        "candidate:1 2 UDP 1 192.0.2.10 55000 typ host",
        "candidate:1 1 UDP 1 192.0.2.10 55000 typ srflx",
        "candidate:1 1 UDP 1 192.0.2.10 55000 typ relay",
        "candidate:1 1 UDP 1 ::1 55000 typ host"}) {
        EXPECT_FALSE(mapped_udp_candidate(candidate, kMapping)) << candidate;
    }
    auto unchanged = kMapping;
    unchanged.external_address = unchanged.internal_address;
    unchanged.external_port = unchanged.internal_port;
    EXPECT_FALSE(mapped_udp_candidate(kHost, unchanged));
}

TEST(IceUdpMapping, RejectsInvalidMappingAndCandidateInjection) {
    for (const auto* address : {"", "not-an-ip", "0.0.0.0", "224.0.0.1", "198.51.100.20\r\na=x"}) {
        auto mapping = kMapping;
        mapping.external_address = address;
        EXPECT_FALSE(mapped_udp_candidate(kHost, mapping));
    }
    auto mapping = kMapping;
    mapping.external_port = 0;
    EXPECT_FALSE(mapped_udp_candidate(kHost, mapping));
    mapping = kMapping;
    mapping.internal_port = 0;
    EXPECT_FALSE(mapped_udp_candidate(kHost, mapping));
    EXPECT_FALSE(mapped_udp_candidate(kHost + "\r\na=x", kMapping));
    EXPECT_FALSE(mapped_udp_candidate("candidate:1 1 UDP invalid 192.0.2.10 55000 typ host", kMapping));
}

TEST(IceUdpMapping, SdpPreservesNativeRoutesCredentialsAndMediaScope) {
    const std::string session = "v=0\r\na=ice-ufrag:unchanged\r\na=ice-pwd:unchanged-for-test\r\n";
    const std::string media = "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=mid:";
    const auto native = "a=" + kHost + "\r\n"
        + "a=candidate:2 1 UDP 1694498815 198.51.100.20 55000 typ srflx raddr 192.0.2.10 rport 55000\r\n";
    const auto sdp = session + media + "0\r\n" + native + "a=end-of-candidates\r\n"
        + media + "1\r\n" + native + "a=end-of-candidates\r\n";
    const auto result = with_mapped_udp_candidates(sdp, kMapping);
    const auto mapped = "a=" + *mapped_udp_candidate(kHost, kMapping) + "\r\n";
    EXPECT_TRUE(result.starts_with(session));
    EXPECT_NE(result.find("198.51.100.20 55000 typ srflx"), std::string::npos);
    const auto first = result.find(mapped);
    ASSERT_NE(first, std::string::npos);
    const auto second = result.find(mapped, first + mapped.size());
    ASSERT_NE(second, std::string::npos);
    EXPECT_EQ(result.find(mapped, second + mapped.size()), std::string::npos);
    EXPECT_LT(first, result.find("a=end-of-candidates"));
    EXPECT_EQ(with_mapped_udp_candidates(result, kMapping), result);
    EXPECT_EQ(with_mapped_udp_candidates(session, kMapping), session);
    auto missing_mapping = kMapping;
    missing_mapping.external_address.clear();
    EXPECT_EQ(with_mapped_udp_candidates(sdp, missing_mapping), sdp);
}
} // namespace
