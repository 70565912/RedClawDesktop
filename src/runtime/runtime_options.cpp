#include "runtime_options_detail.h"

namespace redclaw::runtime {
bool parse_runtime_options(int argc, char** argv, RuntimeOptions* options, std::string* error) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    std::string runtime_config_path;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--help" || arg == "-h") {
            options->help_requested = true;
            return true;
        }
        if (arg == "--runtime-config") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --runtime-config";
                return false;
            }
            runtime_config_path = args[i + 1];
            ++i;
        }
    }

    if (!runtime_config_path.empty()) {
        redclaw::helper::RuntimeProfileData profile;
        if (!redclaw::helper::load_runtime_profile_file(runtime_config_path, &profile, error)) {
            return false;
        }
        if (!apply_runtime_profile(profile, options, error)) {
            return false;
        }
    }

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];

        if (arg == "--help" || arg == "-h") {
            options->help_requested = true;
            return true;
        }

        if (arg == "--runtime-config") {
            ++i;
            continue;
        }

        if (arg == "--role") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --role";
                return false;
            }
            ++i;
            if (!assign_runtime_role(args[i], options, error)) {
                return false;
            }
            continue;
        }

        if (arg == "--target-host") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --target-host";
                return false;
            }
            ++i;
            options->target_host = args[i];
            continue;
        }

        if (arg == "--target-port") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --target-port";
                return false;
            }
            ++i;
            if (!parse_uint16(args[i], &options->target_port)) {
                *error = "invalid --target-port value: " + args[i];
                return false;
            }
            continue;
        }

        if (arg == "--rendezvous-url") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --rendezvous-url";
                return false;
            }
            ++i;
            options->rendezvous_url = args[i];
            continue;
        }

        if (arg == "--session-code") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --session-code";
                return false;
            }
            ++i;
            options->session_code = args[i];
            continue;
        }

        if (arg == "--gui-runtime-stdio") {
            options->gui_runtime_stdio = true;
            continue;
        }
        if (arg == "--run-seconds") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --run-seconds";
                return false;
            }
            ++i;
            if (!parse_uint32(args[i], &options->run_seconds)) {
                *error = "invalid --run-seconds value: " + args[i];
                return false;
            }
            continue;
        }

        if (arg == "--signal-dir") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --signal-dir";
                return false;
            }
            ++i;
            options->signal_dir = args[i];
            continue;
        }

        if (arg == "--signal-transport") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --signal-transport";
                return false;
            }
            ++i;
            options->signal_transport = args[i];
            continue;
        }

        if (arg == "--signal-passphrase") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --signal-passphrase";
                return false;
            }
            ++i;
            options->signal_passphrase = args[i];
            continue;
        }

        if (arg == "--dht-bootstrap") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --dht-bootstrap";
                return false;
            }
            ++i;
            options->dht_bootstrap_nodes.push_back(args[i]);
            continue;
        }

        if (arg == "--network-bind-address") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --network-bind-address";
                return false;
            }
            ++i;
            options->network_bind_address = args[i];
            continue;
        }

        if (arg == "--network-bind-ipv6-address") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --network-bind-ipv6-address";
                return false;
            }
            ++i;
            options->network_bind_ipv6_address = args[i];
            continue;
        }

        if (arg == "--dht-listen-port") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --dht-listen-port";
                return false;
            }
            ++i;
            if (!parse_uint16(args[i], &options->dht_listen_port)) {
                *error = "invalid --dht-listen-port value: " + args[i];
                return false;
            }
            continue;
        }

        if (arg == "--ice-udp-port") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --ice-udp-port";
                return false;
            }
            ++i;
            if (!parse_uint16(args[i], &options->ice_udp_port) || options->ice_udp_port == 0) {
                *error = "invalid --ice-udp-port value: " + args[i];
                return false;
            }
            continue;
        }

        if (arg == "--dht-poll-interval-ms") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --dht-poll-interval-ms";
                return false;
            }
            ++i;
            if (!parse_uint32(args[i], &options->dht_poll_interval_ms)) {
                *error = "invalid --dht-poll-interval-ms value: " + args[i];
                return false;
            }
            continue;
        }

        if (arg == "--dht-publish-retry-ms") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --dht-publish-retry-ms";
                return false;
            }
            ++i;
            if (!parse_uint32(args[i], &options->dht_publish_retry_ms)) {
                *error = "invalid --dht-publish-retry-ms value: " + args[i];
                return false;
            }
            continue;
        }

        if (arg == "--enable-port-mapping") {
            options->enable_port_mapping = true;
            continue;
        }

        if (arg == "--disable-ipv6-candidates") {
            options->enable_ipv6_candidates = false;
            continue;
        }

        if (arg == "--allow-remote-diagnostics") {
            options->allow_remote_diagnostics = true;
            continue;
        }

        if (arg == "--allow-remote-input") {
            options->allow_remote_input = true;
            continue;
        }

        if (arg == "--allow-remote-agent") {
            options->allow_remote_agent = true;
            continue;
        }

        if (arg == "--agent-project-manifest") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --agent-project-manifest";
                return false;
            }
            options->agent_project_manifest = args[++i];
            continue;
        }

        if (arg == "--helper-role") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --helper-role";
                return false;
            }
            ++i;
            options->helper_role = args[i];
            continue;
        }

        if (arg == "--ice-server") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --ice-server";
                return false;
            }
            ++i;
            options->ice_servers.push_back(args[i]);
            continue;
        }

        if (arg == "--ice-server-file") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --ice-server-file";
                return false;
            }
            ++i;
            options->ice_server_file = args[i];
            continue;
        }

        if (arg == "--enable-ice-tcp") {
            options->enable_ice_tcp = true;
            continue;
        }

        if (arg == "--stream-smoke") {
            options->stream_smoke = true;
            continue;
        }

        if (arg == "--stream-require-capture") {
            options->stream_smoke = true;
            options->stream_require_capture = true;
            continue;
        }

        if (arg == "--stream-qa-drop-one-media-fragment") {
            options->stream_qa_drop_one_media_fragment = true;
            continue;
        }

        if (arg == "--stream-qa-incomplete-feedback-class") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --stream-qa-incomplete-feedback-class";
                return false;
            }
            ++i;
            options->stream_qa_incomplete_feedback_class = args[i];
            continue;
        }

        if (arg == "--stream-qa-force-required-channel-close") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --stream-qa-force-required-channel-close";
                return false;
            }
            ++i;
            options->stream_qa_force_required_channel_close = args[i];
            continue;
        }

        if (arg == "--stream-qa-native-size") {
            options->stream_qa_native_size = true;
            continue;
        }
        if (arg == "--input-diagnostics") {
            options->input_diagnostics = true;
            continue;
        }
        if (arg == "--agent-qa-fixture-provider") {
            options->agent_qa_fixture_provider = true;
            continue;
        }
        if (arg == "--agent-qa-force-channel-close-after-event") {
            options->agent_qa_force_channel_close_after_event = true;
            continue;
        }

        if (arg == "--stream-preview-width") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --stream-preview-width";
                return false;
            }
            ++i;
            if (!parse_uint32(args[i], &options->stream_preview_width)) {
                *error = "invalid --stream-preview-width value: " + args[i];
                return false;
            }
            continue;
        }

        if (arg == "--stream-video-max-width") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --stream-video-max-width";
                return false;
            }
            ++i;
            if (!parse_uint32(args[i], &options->stream_video_max_width)) {
                *error = "invalid --stream-video-max-width value: " + args[i];
                return false;
            }
            continue;
        }

        if (arg == "--stream-frame-pipe") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --stream-frame-pipe";
                return false;
            }
            ++i;
            options->stream_frame_pipe = args[i];
            continue;
        }

        if (arg == "--stream-navigation-pipe") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --stream-navigation-pipe";
                return false;
            }
            ++i;
            options->stream_navigation_pipe = args[i];
            continue;
        }

        if (arg == "--log-dir") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --log-dir";
                return false;
            }
            ++i;
            options->log_dir = args[i];
            continue;
        }

        if (arg == "--run-id") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --run-id";
                return false;
            }
            ++i;
            options->run_id = args[i];
            continue;
        }

        if (arg == "--enable-debug-control") {
            options->enable_debug_control = true;
            continue;
        }

        if (arg == "--enable-agent-control") {
            options->enable_agent_control = true;
            continue;
        }

        if (arg == "--debug-control-name") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --debug-control-name";
                return false;
            }
            ++i;
            options->debug_control_name = args[i];
            continue;
        }

        if (arg == "--agent-control-name") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --agent-control-name";
                return false;
            }
            ++i;
            options->agent_control_name = args[i];
            continue;
        }

        if (arg == "--coordination-git-sha") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --coordination-git-sha";
                return false;
            }
            ++i;
            options->coordination_git_sha = args[i];
            continue;
        }

        if (arg == "--coordination-journal-path") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --coordination-journal-path";
                return false;
            }
            ++i;
            options->coordination_journal_path = args[i];
            continue;
        }

        if (arg == "--agent-project-root") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --agent-project-root";
                return false;
            }
            ++i;
            continue;
        }

        if (arg == "--cli") {
            options->force_cli = true;
            continue;
        }

        if (arg == "--gui-auto-start") {
            continue;
        }

        if (arg == "--gui-persist-host-wait") {
            continue;
        }

        if (arg == "--gui-role") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --gui-role";
                return false;
            }
            ++i;
            continue;
        }
        if (arg == "--gui-maintenance-resume") {
            if (i + 1 >= args.size()) { *error = "missing value for --gui-maintenance-resume"; return false; }
            ++i;
            continue; // GUI verifies the owner-private context before using it.
        }

        if (arg == "--signal-timeout-seconds") {
            if (i + 1 >= args.size()) {
                *error = "missing value for --signal-timeout-seconds";
                return false;
            }
            ++i;
            if (!parse_uint32(args[i], &options->signal_timeout_seconds)) {
                *error = "invalid --signal-timeout-seconds value: " + args[i];
                return false;
            }
            continue;
        }

        *error = "unknown argument: " + arg;
        return false;
    }

    if (!options->ice_server_file.empty()
        && !append_ice_servers_from_file(options->ice_server_file, &options->ice_servers, error)) {
        return false;
    }

    return validate_runtime_options(*options, error);
}

}  // namespace redclaw::runtime
