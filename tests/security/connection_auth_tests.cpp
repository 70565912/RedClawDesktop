#include <gtest/gtest.h>
#include <deque>
#include "redclaw/security/connection_auth.h"

namespace {
using namespace redclaw::security;
using Message = redclaw::protocol::StreamControlMessageV1;
ConnectionCredential host_credential(std::string_view password = " password ") {
    std::string verifier;
    EXPECT_TRUE(make_connection_verifier(password, &verifier));
    return {true, verifier};
}
void exchange(ConnectionAuthenticator& host, ConnectionAuthenticator& client,
    std::string client_remote_fingerprint = "HOST", bool tamper = false, std::uint64_t now = 100) {
    auto host_hello = host.open("HOST", "CLIENT", now);
    auto client_hello = client.open("CLIENT", client_remote_fingerprint, now);
    std::deque<std::pair<bool, Message>> pending;
    for (auto& m : host_hello) pending.emplace_back(false, m);
    for (auto& m : client_hello) pending.emplace_back(true, m);
    for (unsigned count = 0; !pending.empty() && count < 30; ++count) {
        auto [to_host, message] = pending.front(); pending.pop_front();
        const auto encoded = redclaw::protocol::serialize_stream_control_message_v1(message);
        auto parsed = redclaw::protocol::parse_stream_control_message_v1(encoded);
        ASSERT_TRUE(parsed.ok) << parsed.error;
        if (tamper && parsed.value.auth_step == "client_final") parsed.value.auth_data.back() = '!';
        for (auto& reply : (to_host ? host : client).receive(parsed.value, now + count))
            pending.emplace_back(!to_host, reply);
    }
}
TEST(ConnectionAuth, CorrectPasswordAndFreshReconnectRequireBothProofs) {
    ConnectionAuthenticator host(host_credential()), client({false, " password "});
    exchange(host, client);
    EXPECT_EQ(host.state(), ConnectionAuthState::kAccepted);
    EXPECT_EQ(client.state(), ConnectionAuthState::kAccepted);
    host.reset(); client.reset();
    EXPECT_EQ(host.state(), ConnectionAuthState::kPending);
    exchange(host, client);
    EXPECT_EQ(host.state(), ConnectionAuthState::kAccepted);
    EXPECT_EQ(client.state(), ConnectionAuthState::kAccepted);
}
TEST(ConnectionAuth, IncorrectCaseWhitespaceEmptyAndChangedPasswordsFail) {
    for (const auto* password : {"password", " Password ", "wrong"}) {
        ConnectionAuthenticator host(host_credential()), client({false, password});
        exchange(host, client); EXPECT_NE(host.state(), ConnectionAuthState::kAccepted);
        EXPECT_NE(client.state(), ConnectionAuthState::kAccepted);
    }
    ConnectionAuthenticator host(host_credential("new")), client({false, "old"});
    exchange(host, client); EXPECT_EQ(host.state(), ConnectionAuthState::kRejected);
    std::string verifier; EXPECT_FALSE(make_connection_verifier("", &verifier));
    EXPECT_FALSE(ConnectionAuthenticator({false, ""}).ready());
}
TEST(ConnectionAuth, TamperedProofAndWrongDtlsEndpointFail) {
    ConnectionAuthenticator host(host_credential()), client({false, " password "});
    exchange(host, client, "OTHER"); EXPECT_EQ(host.state(), ConnectionAuthState::kRejected);
    exchange(host, client, "HOST", true); EXPECT_EQ(host.state(), ConnectionAuthState::kRejected);
}
TEST(ConnectionAuth, LegacyPeerTimeoutAndReplayFailClosed) {
    ConnectionAuthenticator host(host_credential());
    const auto old = host.open("HOST", "CLIENT", 100).front();
    Message legacy; legacy.type = redclaw::protocol::StreamControlMessageTypeV1::kHello;
    host.receive(legacy, 101);
    EXPECT_EQ(host.error(), "protocol_version_incompatible");
    host.open("HOST", "CLIENT", 200); host.tick(30200);
    EXPECT_EQ(host.error(), "connection_auth_timeout");
    host.open("HOST", "CLIENT", 40000);
    host.receive(old, 40001); EXPECT_EQ(host.state(), ConnectionAuthState::kRejected);
}
TEST(ConnectionAuth, PriorConnectionProofCannotAuthenticateFreshChallenge) {
    ConnectionAuthenticator host(host_credential()), client({false, " password "});
    const auto old_host_hello = host.open("HOST", "CLIENT", 100).front();
    const auto old_client_hello = client.open("CLIENT", "HOST", 100).front();
    host.receive(old_client_hello, 101);
    const auto first = client.receive(old_host_hello, 101).front();
    const auto challenge = host.receive(first, 102).front();
    const auto old_proof = client.receive(challenge, 103).front();
    const auto new_host_hello = host.open("HOST", "CLIENT", 200).front();
    const auto new_client_hello = client.open("CLIENT", "HOST", 200).front();
    host.receive(new_client_hello, 201);
    const auto new_first = client.receive(new_host_hello, 201).front();
    host.receive(new_first, 202);
    auto replay = old_proof;
    replay.session_epoch = new_client_hello.session_epoch; // Even rewriting the outer epoch fails.
    host.receive(replay, 203);
    EXPECT_EQ(host.state(), ConnectionAuthState::kRejected);
}
TEST(ConnectionAuth, ExactUnicodePasswordWorksWithoutNormalization) {
    const std::string password = " \xE5\xAF\x86\xE7\xA0\x81 e\xCC\x81 ";
    ConnectionAuthenticator host(host_credential(password)), client({false, password});
    exchange(host, client);
    EXPECT_EQ(host.state(), ConnectionAuthState::kAccepted);
    EXPECT_EQ(client.state(), ConnectionAuthState::kAccepted);
}
TEST(ConnectionAuth, FiveFailuresSurviveResetAndCooldownExpires) {
    ConnectionAuthenticator host(host_credential()), client({false, "wrong"});
    for (unsigned i = 0; i < 5; ++i) exchange(host, client, "HOST", false, 100 + i * 100);
    const auto rejection = host.open("HOST", "CLIENT", 1000);
    EXPECT_EQ(host.error(), "connection_auth_cooldown");
    client.open("CLIENT", "HOST", 1000);
    client.receive(rejection.front(), 1001);
    EXPECT_EQ(client.error(), "connection_auth_cooldown");
    ConnectionAuthenticator correct({false, " password "});
    exchange(host, correct, "HOST", false, 40000);
    EXPECT_EQ(host.state(), ConnectionAuthState::kAccepted);
}
TEST(ConnectionAuth, LocalCredentialFrameHasIndependentVersionAndRoleValidation) {
    auto credential = host_credential(); ConnectionCredential decoded;
    auto frame = serialize_local_connection_credential(credential);
    EXPECT_TRUE(parse_local_connection_credential(frame, &decoded));
    EXPECT_TRUE(decoded.host); EXPECT_EQ(decoded.secret, credential.secret);
    EXPECT_FALSE(parse_local_connection_credential("RCD-LOCAL-AUTH-V1 {}", &decoded));
    EXPECT_FALSE(parse_local_connection_credential(serialize_local_connection_credential({false, ""}), &decoded));
    EXPECT_FALSE(redclaw::protocol::parse_stream_control_message_v1(frame).ok);
}
#ifdef _WIN32
TEST(ConnectionAuth, DpapiRoundTripTamperAndPlaintextFail) {
    std::string encrypted, recovered;
    ASSERT_TRUE(protect_connection_credential(" exact password ", &encrypted));
    EXPECT_EQ(encrypted.find("password"), std::string::npos);
    ASSERT_TRUE(unprotect_connection_credential(encrypted, &recovered));
    EXPECT_EQ(recovered, " exact password ");
    encrypted[encrypted.size() / 2] = encrypted[encrypted.size() / 2] == 'A' ? 'B' : 'A';
    EXPECT_FALSE(unprotect_connection_credential(encrypted, &recovered));
    EXPECT_FALSE(unprotect_connection_credential("plain password", &recovered));
}
#endif
} // namespace
