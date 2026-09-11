#include "runtime_options_detail.h"

namespace redclaw::runtime {
bool apply_runtime_profile(const redclaw::helper::RuntimeProfileData& profile, RuntimeOptions* options, std::string* error) {
    if (profile.role.has_value()) {
        if (!assign_runtime_role(profile.role.value(), options, error)) {
            return false;
        }
    }
    if (profile.target_host.has_value()) {
        options->target_host = profile.target_host.value();
    }
    if (profile.target_port.has_value()) {
        options->target_port = profile.target_port.value();
    }
    if (profile.rendezvous_url.has_value()) {
        options->rendezvous_url = profile.rendezvous_url.value();
    }
    if (profile.session_code.has_value()) {
        options->session_code = profile.session_code.value();
    }
    if (profile.run_seconds.has_value()) {
        options->run_seconds = profile.run_seconds.value();
    }
    if (profile.signal_timeout_seconds.has_value()) {
        options->signal_timeout_seconds = profile.signal_timeout_seconds.value();
    }
    if (profile.signal_dir.has_value()) {
        options->signal_dir = profile.signal_dir.value();
    }
    if (profile.signal_transport.has_value()) {
        options->signal_transport = profile.signal_transport.value();
    }
    if (profile.signal_passphrase.has_value()) {
        options->signal_passphrase = profile.signal_passphrase.value();
    }
    if (!profile.dht_bootstrap_nodes.empty()) {
        options->dht_bootstrap_nodes.insert(
            options->dht_bootstrap_nodes.end(),
            profile.dht_bootstrap_nodes.begin(),
            profile.dht_bootstrap_nodes.end());
    }
    if (profile.network_bind_address.has_value()) {
        options->network_bind_address = profile.network_bind_address.value();
    }
    if (profile.ice_udp_port.has_value()) {
        options->ice_udp_port = profile.ice_udp_port.value();
    }
    if (profile.dht_listen_port.has_value()) {
        options->dht_listen_port = profile.dht_listen_port.value();
    }
    if (profile.dht_poll_interval_ms.has_value()) {
        options->dht_poll_interval_ms = profile.dht_poll_interval_ms.value();
    }
    if (profile.dht_publish_retry_ms.has_value()) {
        options->dht_publish_retry_ms = profile.dht_publish_retry_ms.value();
    }
    if (profile.enable_port_mapping.has_value()) {
        options->enable_port_mapping = profile.enable_port_mapping.value();
    }
    if (profile.enable_ipv6_candidates.has_value()) {
        options->enable_ipv6_candidates = profile.enable_ipv6_candidates.value();
    }
    if (profile.allow_remote_input.has_value()) {
        options->allow_remote_input = profile.allow_remote_input.value();
    }
    if (profile.allow_remote_agent.has_value()) {
        options->allow_remote_agent = profile.allow_remote_agent.value();
    }
    if (profile.agent_project_manifest.has_value()) {
        options->agent_project_manifest = profile.agent_project_manifest.value();
    }
    if (profile.helper_role.has_value()) {
        options->helper_role = profile.helper_role.value();
    }
    if (!profile.ice_servers.empty()) {
        options->ice_servers.insert(options->ice_servers.end(), profile.ice_servers.begin(), profile.ice_servers.end());
    }
    if (profile.enable_ice_tcp.has_value()) {
        options->enable_ice_tcp = profile.enable_ice_tcp.value();
    }
    if (profile.stream_smoke.has_value()) {
        options->stream_smoke = profile.stream_smoke.value();
    }
    if (profile.stream_require_capture.has_value()) {
        options->stream_require_capture = profile.stream_require_capture.value();
        if (options->stream_require_capture) {
            options->stream_smoke = true;
        }
    }
    if (profile.stream_preview_width.has_value()) {
        options->stream_preview_width = profile.stream_preview_width.value();
    }
    if (profile.stream_video_max_width.has_value()) {
        options->stream_video_max_width = profile.stream_video_max_width.value();
    }
    if (profile.cli.has_value()) {
        options->force_cli = profile.cli.value();
    }

    return true;
}

}  // namespace redclaw::runtime
