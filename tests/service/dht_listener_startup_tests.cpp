#include "dht_listen_port_selection.h"
#include "redclaw/service/dht_rendezvous.h"
#include "redclaw/diag/operation_timing.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace {
using namespace redclaw::service::detail;
TEST(DhtListenerStartup, TcpOnlyNeverEstablishesUdpReadiness) {
    DhtListenerStartup startup(100);
    EXPECT_EQ(startup.evaluate(5099), DhtListenerState::kPending);
    EXPECT_EQ(startup.evaluate(5100), DhtListenerState::kFailed);
    startup.udp_ready(52071);
    EXPECT_EQ(startup.evaluate(5200), DhtListenerState::kFailed);
    EXPECT_EQ(startup.port(), 0);
}
TEST(DhtListenerStartup, OneWorkingFamilyIsSufficientAndNotAnOngoingLease) {
    DhtListenerStartup startup(100);
    startup.udp_ready(52071);
    EXPECT_EQ(startup.evaluate(100000), DhtListenerState::kReady);
    EXPECT_EQ(startup.port(), 52071);
    startup.udp_ready(52072);
    EXPECT_EQ(startup.port(), 52071);
}
TEST(DhtListenerStartup, UnconfirmedOrInvalidPortIsNeverReportedAsReady) {
    DhtListenerStartup startup(100);
    EXPECT_EQ(startup.port(), 0);
    for (const int invalid : {-1, 0, 65536}) {
        startup.udp_ready(invalid);
        EXPECT_EQ(startup.state(), DhtListenerState::kPending);
        EXPECT_EQ(startup.port(), 0);
    }
    startup.udp_ready(65535);
    EXPECT_EQ(startup.state(), DhtListenerState::kReady);
    EXPECT_EQ(startup.port(), 65535);
}
TEST(DhtJointPort, RetriesTcpExcludedUdpAllowedAndBoundsExhaustion) {
    unsigned calls = 0;
    auto result = select_dht_listen_port_bounded(0, [&](int) {
        if (++calls == 3) return DhtListenPortSelection{.selected_port = 52071};
        return DhtListenPortSelection{.error_detail = "DHT IPv4 TCP bind failed (10013)"};
    });
    EXPECT_EQ(calls, 3U); EXPECT_EQ(result.probe_attempts, 3U);
    EXPECT_EQ(result.selected_port, 52071); EXPECT_TRUE(result.os_assigned);
    calls = 0;
    result = select_dht_listen_port_bounded(0, [&](int) {
        ++calls; return DhtListenPortSelection{.error_detail = "DHT UDP bind failed"};
    });
    EXPECT_EQ(calls, kDhtPortProbeLimit); EXPECT_EQ(result.selected_port, 0);
    EXPECT_FALSE(result.os_assigned); EXPECT_FALSE(result.error_detail.empty());
}
TEST(DhtJointPort, ExplicitPortFailsOnceAndCannotSilentlyChange) {
    unsigned calls = 0;
    const auto result = select_dht_listen_port_bounded(12345, [&](int requested) {
        EXPECT_EQ(requested, 12345); ++calls;
        return DhtListenPortSelection{.selected_port = 12346};
    });
    EXPECT_EQ(calls, 1U); EXPECT_EQ(result.selected_port, 0);
    EXPECT_FALSE(result.error_detail.empty());
    EXPECT_EQ(select_dht_listen_port_bounded(-1, {}).probe_attempts, 0U);
}
#ifdef _WIN32
TEST(DhtListenerDiagnostics, ReadyListenerReadsAndPendingFetchDoNotQueryNetworkThread) {
    redclaw::service::LibtorrentDhtRendezvousStoreOptions options;
    options.listen_address = "127.0.0.1";
    options.enable_ipv6 = false;
    options.bootstrap_nodes = {"127.0.0.1:1"};
    redclaw::service::LibtorrentDhtRendezvousStore store(options);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int ready_port = 0;
    do {
        store.pump_alerts(std::chrono::milliseconds(5));
        ready_port = store.listen_port();
    } while (!ready_port && std::chrono::steady_clock::now() < deadline);
    ASSERT_GT(ready_port, 0);

    unsigned synchronous_queries = 0;
    const auto previous = redclaw::diag::set_thread_timing_observer({&synchronous_queries,
        [](void* context, redclaw::diag::DiagnosticOperation operation, const redclaw::diag::OperationTimingSample&) {
            if (operation == redclaw::diag::DiagnosticOperation::kDhtListenPort)
                ++*static_cast<unsigned*>(context);
        }});
    const int observed_port = store.listen_port();
    const auto snapshot = store.diagnostics_snapshot();
    std::string error;
    (void)store.fetch(std::string(64, 'a'), "host", 0, std::string(64, 'b'), &error);
    redclaw::diag::set_thread_timing_observer(previous);
    EXPECT_EQ(observed_port, ready_port);
    EXPECT_EQ(snapshot.listen_port, ready_port);
    EXPECT_TRUE(snapshot.listen_ready);
    // A local diagnostic read must not wait for libtorrent's network thread.
    // Real pressure traces reproduce 100+ ms waits in that synchronous query.
    EXPECT_EQ(synchronous_queries, 0U);
}
TEST(DhtJointPort, ActualTcpOccupiedButUdpBindableFailsPreflight) {
    WSADATA data{};
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
    struct Lease {
        SOCKET tcp = INVALID_SOCKET, udp = INVALID_SOCKET;
        ~Lease() { if (tcp != INVALID_SOCKET) closesocket(tcp); if (udp != INVALID_SOCKET) closesocket(udp); WSACleanup(); }
    } lease;
    lease.tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(lease.tcp, INVALID_SOCKET);
    BOOL exclusive = TRUE;
    ASSERT_EQ(setsockopt(lease.tcp, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)), 0);
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto available = select_dht_listen_port(0, "127.0.0.1");
    ASSERT_TRUE(available.error_detail.empty()) << available.error_detail;
    address.sin_port = htons(static_cast<unsigned short>(available.selected_port));
    ASSERT_EQ(bind(lease.tcp, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(lease.tcp, 1), 0);
    int length = sizeof(address);
    ASSERT_EQ(getsockname(lease.tcp, reinterpret_cast<sockaddr*>(&address), &length), 0);
    lease.udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ASSERT_NE(lease.udp, INVALID_SOCKET);
    ASSERT_EQ(bind(lease.udp, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    closesocket(lease.udp); lease.udp = INVALID_SOCKET;
    const auto result = select_dht_listen_port(ntohs(address.sin_port), "127.0.0.1");
    EXPECT_EQ(result.selected_port, 0); EXPECT_EQ(result.probe_attempts, 1U);
    EXPECT_NE(result.error_detail.find("TCP"), std::string::npos);
    redclaw::service::LibtorrentDhtRendezvousStoreOptions options;
    options.listen_address = "127.0.0.1"; options.listen_port = ntohs(address.sin_port); options.enable_ipv6 = false;
    EXPECT_THROW(redclaw::service::LibtorrentDhtRendezvousStore{options}, std::runtime_error);
}
TEST(DhtJointPort, ActualDualFamilyAssignmentReleasesAllProbesAndStartsNativeUdp) {
    const auto result = select_dht_listen_port(0, "127.0.0.1", "::1", true);
    ASSERT_TRUE(result.error_detail.empty()) << result.error_detail;
    ASSERT_GT(result.selected_port, 0); EXPECT_LE(result.probe_attempts, kDhtPortProbeLimit);
    redclaw::service::LibtorrentDhtRendezvousStoreOptions options;
    options.listen_address = "127.0.0.1"; options.listen_ipv6_address = "::1";
    options.listen_port = result.selected_port;
    redclaw::service::LibtorrentDhtRendezvousStore store(options);
    EXPECT_EQ(store.listen_port(), result.selected_port);
    const auto stats = store.diagnostics_snapshot();
    EXPECT_TRUE(stats.listen_ready); EXPECT_FALSE(stats.listen_startup_failed);
}
TEST(DhtJointPort, RepeatedAutomaticAllocationAvoidsUdpExclusions) {
    // Uses production port-zero allocation, not the QA script's explicit-port workaround.
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        const auto selected = select_dht_listen_port(0, "127.0.0.1", "::1", true);
        ASSERT_TRUE(selected.error_detail.empty()) << selected.error_detail;
        EXPECT_TRUE(selected.os_assigned);
        EXPECT_LE(selected.probe_attempts, kDhtPortProbeLimit);
        const auto explicit_check = select_dht_listen_port(selected.selected_port, "127.0.0.1", "::1", true);
        ASSERT_TRUE(explicit_check.error_detail.empty()) << explicit_check.error_detail;
        EXPECT_EQ(explicit_check.selected_port, selected.selected_port);
        EXPECT_FALSE(explicit_check.os_assigned);
    }
}
TEST(DhtJointPort, InvalidExplicitAddressFailsBeforeCreatingSession) {
    EXPECT_FALSE(select_dht_listen_port(12345, "bad-address").error_detail.empty());
    EXPECT_FALSE(select_dht_listen_port(0, "127.0.0.1", "bad-v6", true).error_detail.empty());
}
#endif
} // namespace
