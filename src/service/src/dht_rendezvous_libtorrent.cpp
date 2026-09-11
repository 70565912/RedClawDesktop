#include "redclaw/service/dht_rendezvous.h"
#include "redclaw/service/dht_publish_registry.h"

#include "dht_listen_port_selection.h"
#include "dht_mutable_get_accumulator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <libtorrent/alert.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/session_stats.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/kademlia/ed25519.hpp>
#include <libtorrent/kademlia/item.hpp>

namespace redclaw::service {


namespace {

namespace lt = libtorrent;

constexpr std::string_view kRecordKind = "redclaw-dht-record-v1";
constexpr auto kAlertWaitSlice = std::chrono::milliseconds(50);
constexpr auto kListenPortProbeDelay = std::chrono::milliseconds(100);
constexpr int kListenPortProbeAttempts = 20;
constexpr int kBep44MaxMutableItemBytes = 1000;
// Session codes are meant to stay usable for a long time, so every process run
// that reuses a code must publish above the sequence numbers left on storage
// nodes by earlier runs. Scaling wall-clock seconds leaves room for the
// per-run revisions to stay below the next second's base.
constexpr std::int64_t kPublishSequenceRevisionScale = 1000;
constexpr auto kPublishRepublishAfter = std::chrono::seconds(15);
constexpr std::size_t kMutableFetchCacheLimit = 64;

struct DhtSessionMetricIndices {
    int dht_nodes = -1;
    int dht_node_cache = -1;
};

const DhtSessionMetricIndices& dht_session_metric_indices() {
    static const DhtSessionMetricIndices indices = [] {
        DhtSessionMetricIndices resolved;
        resolved.dht_nodes = lt::find_metric_idx("dht.dht_nodes");
        resolved.dht_node_cache = lt::find_metric_idx("dht.dht_node_cache");
        if (resolved.dht_node_cache < 0) {
            resolved.dht_node_cache = lt::find_metric_idx("dht.dht_nodes_cache");
        }
        return resolved;
    }();
    return indices;
}

std::string trim_copy(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

std::string to_lower_ascii(std::string_view value) {
    std::string lower;
    lower.reserve(value.size());
    for (const char ch : value) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lower;
}

std::optional<std::vector<unsigned char>> hex_decode(std::string_view value) {
    if ((value.size() % 2) != 0) {
        return std::nullopt;
    }

    std::vector<unsigned char> bytes;
    bytes.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        unsigned int parsed = 0;
        std::istringstream in{std::string(value.substr(i, 2))};
        in >> std::hex >> parsed;
        if (!in || parsed > 0xFFU) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<unsigned char>(parsed));
    }
    return bytes;
}

template <typename PublicKeyBytes>
std::string make_publish_state_key(
    const PublicKeyBytes& public_key,
    std::string_view salt,
    std::uint64_t revision) {
    std::string key;
    key.reserve(public_key.size() + salt.size() + 32);
    key.append(public_key.data(), public_key.size());
    key.push_back('\n');
    key.append(salt.data(), salt.size());
    key.push_back('\n');
    key += std::to_string(revision);
    return key;
}

template <typename PublicKeyBytes>
std::string make_fetch_state_key(
    const PublicKeyBytes& public_key,
    std::string_view salt) {
    std::string key;
    key.reserve(public_key.size() + salt.size() + 1);
    key.append(public_key.data(), public_key.size());
    key.push_back('\n');
    key.append(salt.data(), salt.size());
    return key;
}

std::uint64_t current_unix_seconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

std::int64_t make_publish_sequence_base() {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
    return static_cast<std::int64_t>(now) * kPublishSequenceRevisionScale;
}

std::string make_libtorrent_listen_interfaces(
    std::string_view listen_address,
    std::string_view listen_ipv6_address,
    int listen_port,
    bool enable_ipv6) {
    const int port = listen_port > 0 ? listen_port : 0;
    const std::string address = trim_copy(listen_address);
    const std::string ipv6_address = trim_copy(listen_ipv6_address);
    if (!address.empty()) {
        std::string interfaces = address + ":" + std::to_string(port);
        if (enable_ipv6 && !ipv6_address.empty()) {
            interfaces += ",[" + ipv6_address + "]:" + std::to_string(port);
        }
        return interfaces;
    }
    if (enable_ipv6) {
        return "0.0.0.0:" + std::to_string(port) + ",[::]:" + std::to_string(port);
    }
    return "0.0.0.0:" + std::to_string(port);
}

lt::settings_pack make_libtorrent_settings(
    const LibtorrentDhtRendezvousStoreOptions& options,
    bool automatic_listen_port) {
    lt::settings_pack settings;
    settings.set_bool(lt::settings_pack::enable_dht, true);
    settings.set_bool(lt::settings_pack::enable_lsd, false);
    settings.set_bool(lt::settings_pack::enable_upnp, false);
    settings.set_bool(lt::settings_pack::enable_natpmp, false);
    // Hyper-V, WSL, VPN, and container networking reserve sizeable UDP port
    // ranges on Windows. Libtorrent may initially choose one of those ranges
    // for a :0 listener; let the OS choose a usable system port after that
    // bind fails instead of leaving the DHT session permanently offline.
    settings.set_bool(
        lt::settings_pack::listen_system_port_fallback,
        automatic_listen_port);
    settings.set_int(lt::settings_pack::alert_mask, std::numeric_limits<int>::max());
    settings.set_str(
        lt::settings_pack::listen_interfaces,
        make_libtorrent_listen_interfaces(
            options.listen_address,
            options.listen_ipv6_address,
            options.listen_port,
            options.enable_ipv6));

    const auto nodes = normalize_dht_bootstrap_nodes(options.bootstrap_nodes);
    std::ostringstream out;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (i > 0) {
            out << ',';
        }
        out << nodes[i];
    }
    settings.set_str(lt::settings_pack::dht_bootstrap_nodes, out.str());
    return settings;
}

lt::session make_libtorrent_session(
    const LibtorrentDhtRendezvousStoreOptions& options,
    bool automatic_listen_port) {
    lt::session_params params(make_libtorrent_settings(options, automatic_listen_port));
    lt::session session(params);

    for (const std::string& node : normalize_dht_bootstrap_nodes(options.bootstrap_nodes)) {
        const std::size_t split = node.rfind(':');
        if (split == std::string::npos || split == 0 || split + 1 >= node.size()) {
            continue;
        }

        int port = 0;
        const std::string_view port_view(node.data() + split + 1, node.size() - split - 1);
        const auto parsed = std::from_chars(port_view.data(), port_view.data() + port_view.size(), port);
        if (parsed.ec != std::errc{} || parsed.ptr != port_view.data() + port_view.size() || port <= 0 || port > 65535) {
            continue;
        }

        std::string host = node.substr(0, split);
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
            host = host.substr(1, host.size() - 2);
        }
        session.add_dht_node({host, port});
    }

    return session;
}

std::optional<std::pair<lt::dht::public_key, lt::dht::secret_key>> derive_keypair(
    std::string_view routing_token,
    std::string* error_detail) {
    const auto seed_bytes = hex_decode(routing_token);
    if (!seed_bytes.has_value() || seed_bytes->size() != 32) {
        if (error_detail != nullptr) {
            *error_detail = "DHT routing token must be 32 bytes of hex";
        }
        return std::nullopt;
    }

    std::array<char, 32> seed {};
    std::memcpy(seed.data(), seed_bytes->data(), seed.size());
    const auto keypair = lt::dht::ed25519_create_keypair(seed);
    return std::make_pair(std::get<0>(keypair), std::get<1>(keypair));
}

lt::entry make_record_entry(const DhtEncryptedRecord& record) {
    lt::entry entry(lt::entry::dictionary_t);
    auto& dict = entry.dict();
    dict["kind"] = std::string(kRecordKind);
    dict["topic_hex"] = record.topic_hex;
    dict["lane"] = record.lane;
    dict["revision"] = static_cast<lt::entry::integer_type>(record.revision);
    dict["expires_at_unix"] = static_cast<lt::entry::integer_type>(record.expires_at_unix);
    dict["encrypted_blob"] = record.encrypted_blob;
    return entry;
}

std::vector<char> bencode_entry_copy(const lt::entry& entry) {
    std::vector<char> encoded;
    lt::bencode(std::back_inserter(encoded), entry);
    return encoded;
}

std::optional<DhtEncryptedRecord> parse_record_entry(
    const lt::entry& entry,
    std::string_view topic_hex,
    std::string_view lane,
    std::string* error_detail) {
    if (entry.type() != lt::entry::dictionary_t) {
        if (error_detail != nullptr) {
            *error_detail = "libtorrent DHT item is not a dictionary";
        }
        return std::nullopt;
    }

    const auto& dict = entry.dict();
    const auto kind_it = dict.find("kind");
    const auto topic_it = dict.find("topic_hex");
    const auto lane_it = dict.find("lane");
    const auto revision_it = dict.find("revision");
    const auto expires_it = dict.find("expires_at_unix");
    const auto blob_it = dict.find("encrypted_blob");
    if (kind_it == dict.end() || topic_it == dict.end() || lane_it == dict.end()
        || revision_it == dict.end() || expires_it == dict.end() || blob_it == dict.end()) {
        if (error_detail != nullptr) {
            *error_detail = "libtorrent DHT item is missing required fields";
        }
        return std::nullopt;
    }

    if (kind_it->second.type() != lt::entry::string_t || kind_it->second.string() != kRecordKind
        || topic_it->second.type() != lt::entry::string_t || topic_it->second.string() != topic_hex
        || lane_it->second.type() != lt::entry::string_t || lane_it->second.string() != lane
        || revision_it->second.type() != lt::entry::int_t
        || expires_it->second.type() != lt::entry::int_t
        || blob_it->second.type() != lt::entry::string_t) {
        if (error_detail != nullptr) {
            *error_detail = "libtorrent DHT item fields failed validation";
        }
        return std::nullopt;
    }

    if (revision_it->second.integer() <= 0 || expires_it->second.integer() <= 0) {
        if (error_detail != nullptr) {
            *error_detail = "libtorrent DHT item revision or expiry is invalid";
        }
        return std::nullopt;
    }

    DhtEncryptedRecord record;
    record.topic_hex = std::string(topic_hex);
    record.lane = std::string(lane);
    record.encrypted_blob = blob_it->second.string();
    record.revision = static_cast<std::uint64_t>(revision_it->second.integer());
    record.expires_at_unix = static_cast<std::uint64_t>(expires_it->second.integer());
    return record;
}


}  // namespace

struct LibtorrentDhtRendezvousStore::Impl {
    static std::uint64_t steady_ms() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    struct PendingFetchState {
        std::string topic_hex;
        std::string lane;
        std::chrono::steady_clock::time_point started_at;
        detail::DhtMutableGetAccumulator accumulator;
        std::string last_parse_error;
    };

    struct MutableFetchCacheEntry {
        DhtEncryptedRecord record;
        std::chrono::steady_clock::time_point observed_at;
    };

    struct AlertDiagnostics {
        int listen_succeeded_count = 0;
        int listen_failed_count = 0;
        int bootstrap_count = 0;
        int dht_error_count = 0;
        int external_ip_count = 0;
        int dht_log_count = 0;
        int put_alert_count = 0;
        int last_put_num_success = -1;
        std::uint64_t mutable_get_started_count = 0;
        std::uint64_t mutable_get_response_count = 0;
        std::uint64_t mutable_get_authoritative_count = 0;
        std::uint64_t mutable_get_non_authoritative_count = 0;
        std::uint64_t mutable_get_pending_poll_count = 0;
        std::uint64_t mutable_get_cache_hit_count = 0;
        std::int64_t last_mutable_get_elapsed_ms = -1;
        std::int64_t max_mutable_get_elapsed_ms = -1;
        std::int64_t last_mutable_get_sequence = -1;
        int dht_nodes = -1;
        int dht_node_cache = -1;
        bool bootstrap_seen = false;
        std::string last_listen_success;
        std::string last_listen_failure;
        std::string last_dht_error;
        std::string last_external_ip;
        std::vector<std::string> recent_dht_logs;
    };

    explicit Impl(LibtorrentDhtRendezvousStoreOptions requested_options)
        : automatic_listen_port(requested_options.listen_port <= 0)
        , port_selection(detail::select_dht_listen_port(
              requested_options.listen_port,
              requested_options.listen_address, requested_options.listen_ipv6_address,
              requested_options.enable_ipv6))
        , options(std::move(requested_options))
        , session([&]() {
              if (!port_selection.error_detail.empty()) throw std::runtime_error(port_selection.error_detail);
              if (automatic_listen_port && port_selection.selected_port > 0) {
                  options.listen_port = port_selection.selected_port;
              }
              return make_libtorrent_session(options, automatic_listen_port);
          }()) {}

    void observe_alert(const lt::alert& base_alert) {
        if (const auto* listen_succeeded = lt::alert_cast<lt::listen_succeeded_alert>(&base_alert); listen_succeeded != nullptr) {
            ++diagnostics.listen_succeeded_count;
            diagnostics.last_listen_success = listen_succeeded->message();
            if (listen_succeeded->socket_type == lt::socket_type_t::utp)
                listener_startup.udp_ready(listen_succeeded->port);
        }
        if (const auto* listen_failed = lt::alert_cast<lt::listen_failed_alert>(&base_alert); listen_failed != nullptr) {
            ++diagnostics.listen_failed_count;
            diagnostics.last_listen_failure = listen_failed->message();
        }
        if (const auto* bootstrap = lt::alert_cast<lt::dht_bootstrap_alert>(&base_alert); bootstrap != nullptr) {
            (void)bootstrap;
            ++diagnostics.bootstrap_count;
            diagnostics.bootstrap_seen = true;
        }
        if (const auto* dht_error = lt::alert_cast<lt::dht_error_alert>(&base_alert); dht_error != nullptr) {
            ++diagnostics.dht_error_count;
            diagnostics.last_dht_error = dht_error->message();
        }
        if (const auto* external_ip = lt::alert_cast<lt::external_ip_alert>(&base_alert); external_ip != nullptr) {
            ++diagnostics.external_ip_count;
            diagnostics.last_external_ip = external_ip->external_address.to_string();
        }
        if (const auto* put_alert = lt::alert_cast<lt::dht_put_alert>(&base_alert); put_alert != nullptr) {
            ++diagnostics.put_alert_count;
            diagnostics.last_put_num_success = put_alert->num_success;
            (void)publishes->complete(std::string(put_alert->public_key.data(), put_alert->public_key.size()),
                put_alert->salt, put_alert->seq, put_alert->num_success > 0, steady_ms());
        }
        if (const auto* get_alert = lt::alert_cast<lt::dht_mutable_item_alert>(&base_alert); get_alert != nullptr) {
            const std::string fetch_key = make_fetch_state_key(get_alert->key, get_alert->salt);
            const auto fetch_it = pending_fetches.find(fetch_key);
            if (fetch_it != pending_fetches.end()) {
                auto& fetch_state = fetch_it->second;
                std::optional<DhtEncryptedRecord> record;
                if (get_alert->item.type() != lt::entry::undefined_t) {
                    std::string parse_error;
                    record = parse_record_entry(
                        get_alert->item,
                        fetch_state.topic_hex,
                        fetch_state.lane,
                        &parse_error);
                    if (!record.has_value() && get_alert->authoritative) {
                        fetch_state.last_parse_error = std::move(parse_error);
                    } else if (record.has_value() && get_alert->authoritative) {
                        fetch_state.last_parse_error.clear();
                    }
                } else if (get_alert->authoritative) {
                    fetch_state.last_parse_error.clear();
                }

                fetch_state.accumulator.observe(
                    get_alert->seq,
                    get_alert->authoritative,
                    std::move(record));
                ++diagnostics.mutable_get_response_count;
                if (get_alert->authoritative) {
                    ++diagnostics.mutable_get_authoritative_count;
                } else {
                    ++diagnostics.mutable_get_non_authoritative_count;
                }
                update_fetch_elapsed_diagnostics(fetch_state);
            }
        }
        if (const auto* dht_log = lt::alert_cast<lt::dht_log_alert>(&base_alert); dht_log != nullptr) {
            ++diagnostics.dht_log_count;
            diagnostics.recent_dht_logs.push_back(dht_log->log_message());
            if (diagnostics.recent_dht_logs.size() > 4) {
                diagnostics.recent_dht_logs.erase(diagnostics.recent_dht_logs.begin());
            }
        }
        if (const auto* session_stats = lt::alert_cast<lt::session_stats_alert>(&base_alert); session_stats != nullptr) {
            const auto counters = session_stats->counters();
            const auto counters_size = counters.size();
            const auto& indices = dht_session_metric_indices();
            if (indices.dht_nodes >= 0 && indices.dht_nodes < counters_size) {
                diagnostics.dht_nodes = static_cast<int>(counters[indices.dht_nodes]);
            }
            if (indices.dht_node_cache >= 0 && indices.dht_node_cache < counters_size) {
                diagnostics.dht_node_cache = static_cast<int>(counters[indices.dht_node_cache]);
            }
        }
    }

    [[nodiscard]] std::string build_diagnostics_summary() const {
        std::ostringstream out;
        // session.listen_port() synchronously dispatches to libtorrent's network
        // thread. Even a pending-fetch diagnostic must stay local to this owner.
        out << "listen_port=" << listener_startup.port()
            << " listen_port_probe_attempts=" << port_selection.probe_attempts
            << " listen_port_auto_selected="
            << (port_selection.os_assigned ? "true" : "false")
            << " bootstrap_seen=" << (diagnostics.bootstrap_seen ? "true" : "false")
            << " bootstrap_count=" << diagnostics.bootstrap_count
            << " listen_ok=" << diagnostics.listen_succeeded_count
            << " listen_failed=" << diagnostics.listen_failed_count
            << " dht_errors=" << diagnostics.dht_error_count
            << " put_alerts=" << diagnostics.put_alert_count
            << " last_put_num_success=" << diagnostics.last_put_num_success
            << " mutable_get_started=" << diagnostics.mutable_get_started_count
            << " mutable_get_responses=" << diagnostics.mutable_get_response_count
            << " mutable_get_authoritative=" << diagnostics.mutable_get_authoritative_count
            << " mutable_get_non_authoritative=" << diagnostics.mutable_get_non_authoritative_count
            << " mutable_get_pending_polls=" << diagnostics.mutable_get_pending_poll_count
            << " mutable_get_cache_hits=" << diagnostics.mutable_get_cache_hit_count
            << " mutable_get_pending=" << pending_fetches.size()
            << " last_mutable_get_elapsed_ms=" << diagnostics.last_mutable_get_elapsed_ms
            << " max_mutable_get_elapsed_ms=" << diagnostics.max_mutable_get_elapsed_ms
            << " last_mutable_get_sequence=" << diagnostics.last_mutable_get_sequence;
        if (!diagnostics.last_external_ip.empty()) {
            out << " external_ip=" << diagnostics.last_external_ip;
        }
        if (!diagnostics.last_listen_success.empty()) {
            out << " last_listen_success=" << diagnostics.last_listen_success;
        }
        if (!diagnostics.last_listen_failure.empty()) {
            out << " last_listen_failure=" << diagnostics.last_listen_failure;
        }
        if (!diagnostics.last_dht_error.empty()) {
            out << " last_dht_error=" << diagnostics.last_dht_error;
        }
        if (!diagnostics.recent_dht_logs.empty()) {
            out << " recent_dht_log=" << diagnostics.recent_dht_logs.back();
        }
        return out.str();
    }

    [[nodiscard]] detail::DhtListenerState listener_state(std::string* error_detail = nullptr) {
        // Consume queued success before evaluating the deadline. A delayed owner
        // must not classify a listener that was already ready as a startup failure.
        if (listener_startup.state() == detail::DhtListenerState::kPending) {
            pop_alerts_locked();
            if (alert_cursor < pending_alerts.size()) {
                if (error_detail) *error_detail = "DHT listener alerts pending";
                return detail::DhtListenerState::kPending;
            }
        }
        const auto state = listener_startup.evaluate(steady_ms());
        if (error_detail && state != detail::DhtListenerState::kReady)
            *error_detail = state == detail::DhtListenerState::kFailed
                ? "DHT UDP listener startup failed; no publication submitted"
                : "DHT UDP listener starting";
        return state;
    }

    [[nodiscard]] LibtorrentDhtDiagnosticsSnapshot diagnostics_snapshot() {
        LibtorrentDhtDiagnosticsSnapshot snapshot;
        const auto publish_stats = publishes->stats();
        snapshot.publish_records = publish_stats.records;
        snapshot.publish_in_flight = publish_stats.in_flight;
        snapshot.publish_records_peak = publish_stats.peak;
        snapshot.publish_late_events = publish_stats.late_events;
        snapshot.publish_backpressure = publish_stats.backpressure;
        snapshot.listen_ready = listener_state() == detail::DhtListenerState::kReady;
        snapshot.listen_port = listener_startup.port();
        snapshot.listen_port_auto_selected = port_selection.os_assigned;
        snapshot.listen_startup_failed = listener_state() == detail::DhtListenerState::kFailed;
        snapshot.listen_port_probe_attempts = port_selection.probe_attempts;
        snapshot.listen_port_selection_error = port_selection.error_detail;
        snapshot.listen_succeeded_count = diagnostics.listen_succeeded_count;
        snapshot.listen_failed_count = diagnostics.listen_failed_count;
        snapshot.bootstrap_count = diagnostics.bootstrap_count;
        snapshot.dht_error_count = diagnostics.dht_error_count;
        snapshot.external_ip_count = diagnostics.external_ip_count;
        snapshot.dht_log_count = diagnostics.dht_log_count;
        snapshot.put_alert_count = diagnostics.put_alert_count;
        snapshot.last_put_num_success = diagnostics.last_put_num_success;
        snapshot.mutable_get_started_count = diagnostics.mutable_get_started_count;
        snapshot.mutable_get_response_count = diagnostics.mutable_get_response_count;
        snapshot.mutable_get_authoritative_count = diagnostics.mutable_get_authoritative_count;
        snapshot.mutable_get_non_authoritative_count = diagnostics.mutable_get_non_authoritative_count;
        snapshot.mutable_get_pending_poll_count = diagnostics.mutable_get_pending_poll_count;
        snapshot.mutable_get_cache_hit_count = diagnostics.mutable_get_cache_hit_count;
        snapshot.mutable_get_pending_count = static_cast<int>(pending_fetches.size());
        snapshot.last_mutable_get_elapsed_ms = diagnostics.last_mutable_get_elapsed_ms;
        snapshot.max_mutable_get_elapsed_ms = diagnostics.max_mutable_get_elapsed_ms;
        snapshot.last_mutable_get_sequence = diagnostics.last_mutable_get_sequence;
        snapshot.dht_nodes = diagnostics.dht_nodes;
        snapshot.dht_node_cache = diagnostics.dht_node_cache;
        snapshot.bootstrap_seen = diagnostics.bootstrap_seen;
        snapshot.listen_address = options.listen_address;
        snapshot.listen_interfaces = make_libtorrent_listen_interfaces(
            options.listen_address,
            options.listen_ipv6_address,
            options.listen_port,
            options.enable_ipv6);
        snapshot.last_listen_success = diagnostics.last_listen_success;
        snapshot.last_listen_failure = diagnostics.last_listen_failure;
        snapshot.last_dht_error = diagnostics.last_dht_error;
        snapshot.last_external_ip = diagnostics.last_external_ip;
        snapshot.recent_dht_logs = diagnostics.recent_dht_logs;
        snapshot.bootstrap_nodes = normalize_dht_bootstrap_nodes(options.bootstrap_nodes);
        return snapshot;
    }

    void pop_alerts_locked(std::string* error_detail = nullptr) {
        // libtorrent owns these pointers until the next pop_alerts call. Keep
        // that call deferred while the cursor has unprocessed events.
        if (alert_cursor == pending_alerts.size()) {
            pending_alerts.clear();
            session.pop_alerts(&pending_alerts);
            alert_cursor = 0;
        }
        const auto started = std::chrono::steady_clock::now();
        for (unsigned handled = 0; handled < 64 && alert_cursor < pending_alerts.size(); ++handled) {
            if (std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(5)) break;
            lt::alert* alert = pending_alerts[alert_cursor++];
            observe_alert(*alert);
            if (const auto* session_error = lt::alert_cast<lt::session_error_alert>(alert); session_error != nullptr && error_detail != nullptr) {
                *error_detail = session_error->message();
            }
            if (const auto* listen_failed = lt::alert_cast<lt::listen_failed_alert>(alert); listen_failed != nullptr && error_detail != nullptr) {
                *error_detail = listen_failed->message();
            }
        }
    }

    void wait_for_alerts_locked(std::chrono::milliseconds max_wait, std::string* error_detail = nullptr) {
        if (max_wait.count() > 0 && alert_cursor == pending_alerts.size()) {
            session.wait_for_alert(max_wait);
        }
        pop_alerts_locked(error_detail);
    }

    [[nodiscard]] std::int64_t desired_sequence(std::uint64_t revision) const {
        return sequence_base + static_cast<std::int64_t>(revision);
    }

    void update_fetch_elapsed_diagnostics(const PendingFetchState& state) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - state.started_at)
                                 .count();
        diagnostics.last_mutable_get_elapsed_ms = elapsed;
        diagnostics.max_mutable_get_elapsed_ms = std::max(
            diagnostics.max_mutable_get_elapsed_ms,
            elapsed);
        diagnostics.last_mutable_get_sequence = state.accumulator.authoritative_sequence().value_or(
            state.accumulator.highest_observed_sequence().value_or(-1));
    }

    void prune_fetch_cache(std::uint64_t now_unix) {
        for (auto it = fetch_cache.begin(); it != fetch_cache.end();) {
            if (it->second.record.expires_at_unix <= now_unix) {
                it = fetch_cache.erase(it);
            } else {
                ++it;
            }
        }
        while (fetch_cache.size() > kMutableFetchCacheLimit) {
            const auto oldest = std::min_element(
                fetch_cache.begin(),
                fetch_cache.end(),
                [](const auto& left, const auto& right) {
                    return left.second.observed_at < right.second.observed_at;
                });
            if (oldest == fetch_cache.end()) {
                break;
            }
            fetch_cache.erase(oldest);
        }
    }

    bool automatic_listen_port = false;
    detail::DhtListenPortSelection port_selection;
    LibtorrentDhtRendezvousStoreOptions options;
    const std::int64_t sequence_base = make_publish_sequence_base();
    mutable std::mutex mutex;
    detail::DhtListenerStartup listener_startup{steady_ms()};
    lt::session session;
    AlertDiagnostics diagnostics;
    std::shared_ptr<detail::DhtPublishRegistry> publishes = std::make_shared<detail::DhtPublishRegistry>();
    std::vector<lt::alert*> pending_alerts;
    std::size_t alert_cursor = 0;
    std::unordered_map<std::string, PendingFetchState> pending_fetches;
    std::unordered_map<std::string, MutableFetchCacheEntry> fetch_cache;
};

LibtorrentDhtRendezvousStore::LibtorrentDhtRendezvousStore(LibtorrentDhtRendezvousStoreOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

LibtorrentDhtRendezvousStore::~LibtorrentDhtRendezvousStore() = default;

void LibtorrentDhtRendezvousStore::advance() {
    std::lock_guard lock(impl_->mutex);
    impl_->pop_alerts_locked();
}

DhtPublishResult LibtorrentDhtRendezvousStore::publish(
    const DhtEncryptedRecord& record,
    std::string_view routing_token,
    std::string* error_detail) {
    if (record.topic_hex.empty() || record.lane.empty() || record.encrypted_blob.empty() || record.revision == 0) {
        if (error_detail != nullptr) {
            *error_detail = "DHT record is incomplete";
        }
        return false;
    }

    const auto keypair = derive_keypair(routing_token, error_detail);
    if (!keypair.has_value()) {
        return false;
    }

    const lt::dht::public_key public_key = keypair->first;
    const lt::dht::secret_key secret_key = keypair->second;
    const lt::entry data = make_record_entry(record);
    std::string encoded_value;
    lt::bencode(std::back_inserter(encoded_value), data);
    if (encoded_value.size() > static_cast<std::size_t>(kBep44MaxMutableItemBytes)) {
        if (error_detail != nullptr) {
            *error_detail = "libtorrent DHT mutable item exceeds BEP44 value limit; encoded_value_bytes="
                + std::to_string(encoded_value.size())
                + " encrypted_blob_bytes="
                + std::to_string(record.encrypted_blob.size())
                + " max_value_bytes="
                + std::to_string(kBep44MaxMutableItemBytes);
        }
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->listener_state() != detail::DhtListenerState::kReady) {
        impl_->pop_alerts_locked();
        const auto readiness = impl_->listener_state(error_detail);
        if (readiness != detail::DhtListenerState::kReady)
            return readiness == detail::DhtListenerState::kFailed
                ? DhtPublishState::kPermanentFailure : DhtPublishState::kPending;
    }
    const auto now = Impl::steady_ms();
    const auto unix_now = current_unix_seconds();
    if (record.expires_at_unix <= unix_now) {
        if (error_detail) *error_detail = "DHT publication record expired";
        return {DhtPublishState::kPermanentFailure, DhtPublishFailure::kExpired};
    }
    const auto expires_ms = now + std::min<std::uint64_t>(record.expires_at_unix - unix_now, 3600) * 1000;
    const auto desired_sequence = impl_->desired_sequence(record.revision);
    const auto publish_key = make_publish_state_key(public_key.bytes, record.topic_hex, record.revision);
    const auto lease = impl_->publishes->begin(publish_key,
        std::string(public_key.bytes.data(), public_key.bytes.size()), record.topic_hex,
        now, expires_ms, static_cast<std::uint64_t>(std::max(1, impl_->options.publish_timeout_milliseconds)),
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(kPublishRepublishAfter).count()));
    if (error_detail) {
        *error_detail = lease.result.state == DhtPublishState::kPending ? "DHT put pending"
            : lease.result.state == DhtPublishState::kRetryableFailure ? "DHT publication capacity backpressure" : "";
    }
    if (!lease.submit) return lease.result;
    auto registry = impl_->publishes;
    impl_->session.dht_put_item(public_key.bytes,
        [data, public_key, secret_key, desired_sequence, registry, publish_key, attempt = lease.attempt](
            lt::entry& value, std::array<char, 64>& signature, std::int64_t& seq, const std::string& salt) {
            value = data;
            const auto next_sequence = std::max<std::int64_t>(desired_sequence, seq + 1);
            seq = next_sequence;
            const auto encoded = bencode_entry_copy(value);
            signature = lt::dht::sign_mutable_item(
                lt::span<char const>(encoded.data(), encoded.size()),
                lt::span<char const>(salt.data(), salt.size()),
                lt::dht::sequence_number(next_sequence), public_key, secret_key).bytes;
            (void)registry->signed_sequence(publish_key, attempt, next_sequence);
        }, record.topic_hex);
    return DhtPublishState::kPending;
}

std::optional<DhtEncryptedRecord> LibtorrentDhtRendezvousStore::fetch(
    std::string_view topic_hex,
    std::string_view lane,
    std::uint64_t after_revision,
    std::string_view routing_token,
    std::string* error_detail) {
    if (error_detail != nullptr) {
        error_detail->clear();
    }
    const auto keypair = derive_keypair(routing_token, error_detail);
    if (!keypair.has_value()) {
        return std::nullopt;
    }

    const lt::dht::public_key public_key = keypair->first;
    const std::string fetch_key = make_fetch_state_key(public_key.bytes, topic_hex);

    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::string wait_error;
    impl_->pop_alerts_locked(&wait_error);
    if (impl_->listener_state(error_detail) != detail::DhtListenerState::kReady) return std::nullopt;
    const std::uint64_t now_unix = current_unix_seconds();
    impl_->prune_fetch_cache(now_unix);

    const auto cached_it = impl_->fetch_cache.find(fetch_key);
    if (cached_it != impl_->fetch_cache.end()
        && cached_it->second.record.revision > after_revision) {
        ++impl_->diagnostics.mutable_get_cache_hit_count;
        return cached_it->second.record;
    }

    auto finalize_authoritative_result = [&](bool* finalized) -> std::optional<DhtEncryptedRecord> {
        if (finalized != nullptr) {
            *finalized = false;
        }
        const auto pending_it = impl_->pending_fetches.find(fetch_key);
        if (pending_it == impl_->pending_fetches.end()
            || !pending_it->second.accumulator.authoritative_received()) {
            return std::nullopt;
        }
        if (finalized != nullptr) {
            *finalized = true;
        }

        impl_->update_fetch_elapsed_diagnostics(pending_it->second);
        const auto record = pending_it->second.accumulator.authoritative_record();
        const std::string parse_error = pending_it->second.last_parse_error;
        impl_->pending_fetches.erase(pending_it);

        if (!record.has_value()) {
            impl_->fetch_cache.erase(fetch_key);
            if (error_detail != nullptr) {
                *error_detail = parse_error;
            }
            return std::nullopt;
        }
        if (record->expires_at_unix <= current_unix_seconds()) {
            impl_->fetch_cache.erase(fetch_key);
            if (error_detail != nullptr) {
                *error_detail = "DHT authoritative mutable record expired";
            }
            return std::nullopt;
        }

        impl_->fetch_cache[fetch_key] = {
            *record,
            std::chrono::steady_clock::now(),
        };
        impl_->prune_fetch_cache(current_unix_seconds());
        if (record->revision <= after_revision) {
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return std::nullopt;
        }
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return record;
    };

    auto consume_latest_verified_result = [&]() -> std::optional<DhtEncryptedRecord> {
        const auto pending_it = impl_->pending_fetches.find(fetch_key);
        if (pending_it == impl_->pending_fetches.end()) {
            return std::nullopt;
        }
        const auto& record = pending_it->second.accumulator.latest_verified_record();
        if (!record.has_value() || record->expires_at_unix <= current_unix_seconds()) {
            return std::nullopt;
        }

        const auto existing_it = impl_->fetch_cache.find(fetch_key);
        if (existing_it == impl_->fetch_cache.end()
            || record->revision >= existing_it->second.record.revision) {
            impl_->fetch_cache[fetch_key] = {
                *record,
                std::chrono::steady_clock::now(),
            };
            impl_->prune_fetch_cache(current_unix_seconds());
        }
        if (record->revision <= after_revision) {
            return std::nullopt;
        }
        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return record;
    };

    bool finalized = false;
    const auto completed_before_wait = finalize_authoritative_result(&finalized);
    if (finalized) {
        return completed_before_wait;
    }
    if (const auto verified_before_wait = consume_latest_verified_result();
        verified_before_wait.has_value()) {
        return verified_before_wait;
    }

    const auto pending_it = impl_->pending_fetches.find(fetch_key);
    if (pending_it == impl_->pending_fetches.end()) {
        Impl::PendingFetchState state;
        state.topic_hex = std::string(topic_hex);
        state.lane = std::string(lane);
        state.started_at = std::chrono::steady_clock::now();
        impl_->pending_fetches.emplace(fetch_key, std::move(state));
        ++impl_->diagnostics.mutable_get_started_count;
        impl_->session.dht_get_item(public_key.bytes, std::string(topic_hex));
    }

    const auto fetch_deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(impl_->options.fetch_timeout_milliseconds);
    while (std::chrono::steady_clock::now() < fetch_deadline) {
        impl_->wait_for_alerts_locked(std::min(kAlertWaitSlice,
            std::chrono::duration_cast<std::chrono::milliseconds>(fetch_deadline - std::chrono::steady_clock::now())), &wait_error);
        const auto pending = impl_->pending_fetches.find(fetch_key);
        if (pending != impl_->pending_fetches.end() && pending->second.accumulator.response_count() != 0) break;
    }

    const auto completed_after_wait = finalize_authoritative_result(&finalized);
    if (finalized) {
        return completed_after_wait;
    }
    if (const auto verified_after_wait = consume_latest_verified_result();
        verified_after_wait.has_value()) {
        return verified_after_wait;
    }

    const auto still_pending_it = impl_->pending_fetches.find(fetch_key);
    if (still_pending_it == impl_->pending_fetches.end()) {
        return std::nullopt;
    }
    ++impl_->diagnostics.mutable_get_pending_poll_count;
    impl_->update_fetch_elapsed_diagnostics(still_pending_it->second);
    if (error_detail != nullptr) {
        *error_detail = wait_error.empty() ? "libtorrent DHT mutable get pending" : wait_error;
        *error_detail += "; authoritative=false responses="
            + std::to_string(still_pending_it->second.accumulator.response_count())
            + " non_authoritative="
            + std::to_string(still_pending_it->second.accumulator.non_authoritative_count())
            + "; "
            + impl_->build_diagnostics_summary();
    }
    return std::nullopt;
}

int LibtorrentDhtRendezvousStore::listen_port() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (int attempt = 0; impl_->listener_state() == detail::DhtListenerState::kPending
        && attempt < kListenPortProbeAttempts; ++attempt) {
        impl_->wait_for_alerts_locked(kListenPortProbeDelay);
    }
    return impl_->listener_state() == detail::DhtListenerState::kReady ? impl_->listener_startup.port() : 0;
}

void LibtorrentDhtRendezvousStore::pump_alerts(std::chrono::milliseconds max_wait) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->session.post_session_stats();
    const auto deadline = std::chrono::steady_clock::now() + max_wait;
    do {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        impl_->wait_for_alerts_locked(std::max(std::chrono::milliseconds::zero(), std::min(remaining, kAlertWaitSlice)));
    } while (std::chrono::steady_clock::now() < deadline);
}

LibtorrentDhtDiagnosticsSnapshot LibtorrentDhtRendezvousStore::diagnostics_snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->diagnostics_snapshot();
}

}  // namespace redclaw::service
