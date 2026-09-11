# Adaptive Desktop Stream Control v1

## P0-DS-08R5 bounded recovery (2026-09-05)

Host encoder admission is event-driven through `HostStreamWorkCoordinator`: channel/session, source, capacity, latest frame and pacing must all be ready. Pacer capacity callbacks run outside its lock. Failure invalidates queued dependent P frames; connection and local recovery generations prevent old completions clearing a newer recovery. Complete IDR transmission releases normal predictive sending immediately, without waiting for a displayable ACK.

Recovery IDRs (startup, dependency, static reference or geometry) have a 10-second absolute send budget; other frames retain five seconds. Budget revision updates recompute remaining work within the original hard limit. Typed admission separates capacity/recovery waits from infeasible budgets. Repeated infeasibility backs off at 1/2/4/8/16/30 seconds; writable alone cannot reset this. A provisional probe needs two fresh low-queue RTT samples, open media and empty buffer/in-flight state, is capped at 64 KiB without a new media ACK, and can start at most once per ten seconds. Only media feedback confirms the raised rate; ping is not bandwidth evidence.

The existing `source_activity_state` control envelope carries the orthogonal boolean `media_budget_waiting` (absent defaults false). Host publishes entry/exit after bounded admission changes; Controller displays a bandwidth/retry explanation and retains its last clear frame. It does not equate this state with disconnection or static capture. This adds no media framing or payload queue.

Receiver timing is monotonic, with absolute IDR/ordinary reassembly ceilings 10.5/5.5 seconds. IDRs making fragment progress suppress repeated recovery requests until the no-progress threshold or absolute limit; explicit missing fragments still break dependencies immediately. First-media diagnosis allows 20 seconds. The 256-entry metadata ring records first actual send, terminal state and sent fragment count, never payloads. Two-slot/latest-only, no generic retransmit/B frames, resolution and clarity floor remain unchanged. These contracts require synchronized Host/Controller deployment; local test results do not replace physical acceptance.

`P0-DS-08` separates real-time media from reliable session control while keeping both on one WebRTC `PeerConnection`.

## Channel contract

| Kind | Label | Delivery | Payload |
|---|---|---|---|
| Media | `redclaw-media-v1` | Ordered partial reliability, zero retransmits | Binary encoded-video fragments only |
| Control | `redclaw-control-v1` | Reliable and ordered | Whole-message Zstd-compressed Protobuf, binary |

`IceConnectivityWrapper` exposes channel-kind-specific send, open/closed callbacks, message callbacks, and transport statistics. Unknown or duplicate channel labels are closed. Reconnect replaces the `PeerConnection`; generation checks discard late callbacks from an old peer or channel. The old single-channel mixed path is not retained.

The media channel sets libdatachannel `Reliability::Type::Rexmit` with `rexmit=0`. Delivered fragments retain ordering, while a lost SCTP message can be skipped instead of forcing obsolete desktop frames to retransmit behind it. The runtime also rejects text received on the media channel. The former Hex-encoded video-frame and RGB synthetic-preview formats have been removed; a capture failure waits for the next real frame instead of injecting a diagnostic placeholder into media statistics.

The synchronized media-fragment binary header is version 3. Every fragment carries `rate_revision` plus a non-zero `transport_sequence` allocated immediately before the actual DataChannel send. Host and Controller must therefore be upgraded together; old fragment versions are rejected and there is no long-lived compatibility path.

The receiver abandons an incomplete frame when the next frame starts. A sequence gap is counted once for that frame; later fragments from the already-discarded frame are ignored, and the next complete frame starts recovery immediately. This prevents one lost fragment from amplifying into one decode failure per remaining fragment.

## Control envelope

Every control message contains:

- `schema_version=1`
- `session_epoch`
- monotonically increasing `message_id`
- `sent_at_ms`
- typed message name and typed fields

Supported types are `hello`, `capabilities`, `viewport_request`, `stream_target_applied`, `source_activity_state`, `receiver_network_stats`, `media_transport_feedback`, `playback_starvation`, `keyframe_request`, `ping`, `pong`, `remote_log_request`, `remote_log_chunk`, `remote_log_complete`, `remote_log_error`, `input_capabilities`, `input_control_request`, `input_control_status`, `input_batch`, `input_state_sync`, and `input_release_all`. The input extension is specified in [Ordinary Desktop Remote Input v1](ordinary-desktop-remote-input-v1.md).

`playback_starvation` is retained as the v1 wire name, but it is a receiver evidence report rather than a cadence command. It reports `incomplete_frame_id`, `latest_received_frame_id`, `latest_complete_frame_id`, `latest_complete_keyframe_id`, `observed_rate_revision`, a bounded reason, and a monotonic observation count. `target_fps` is not accepted as authority. Host rejects inconsistent identifiers and remains the sole owner of FPS and bitrate decisions.

Host retains a 256-entry metadata-only sent-frame ring containing `frame_id`, steady-clock send time, `rate_revision`, FPS, bitrate, and keyframe status. No encoded payload is retained, so the two-frame media-cache limit remains unchanged. FPS, bitrate, or resolution target changes increment `rate_revision`; each encoded fragment carries the revision that produced it. Host correlates damaged-frame evidence with this ring and computes `feedback_age` using its own monotonic clock. Fresh, delayed, expired, unknown, clock-invalid, and stale-revision classes remain observable, but they no longer apply a second rate multiplier beside transport feedback. Decoder dependency damage requests the latest IDR independently of the single congestion decision.

`message_id` and `sent_at_ms` are assigned under the same per-session send lock immediately before validation, serialization, and DataChannel send. Creating a message on a worker thread does not reserve its wire ID. This makes the reliable-channel wire order and replay-guard order identical even when keyframe recovery, input, heartbeat, and diagnostics originate concurrently.

The remote parser rejects messages larger than 64 KiB, invalid ranges, an epoch change without a new `hello`, replayed/non-increasing IDs, and old-epoch messages after reconnect. Receiver statistics are untrusted and range/consistency checked before use.

## Application transport feedback and pacing

`media_transport_feedback` is an application-message analogue of WebRTC transport-wide feedback; it is not RTP/UDP TWCC. Controller records delivered media-message sequence and its local steady arrival time, then sends a reliable feedback batch every 100 ms or after 32 new arrivals. A batch has a monotonic `feedback_id`, the observed rate revision, and at most 64 strictly increasing arrivals. Parser validation bounds the batch, message size, sequence/time order, revision, epoch, and replay. Invalid feedback only increments diagnostics and cannot change the Host rate.

Host retains at most 4096 payload-free sent-packet records. `MediaTransportEstimator` joins arrivals to those records and publishes a 500 ms acknowledged bitrate, transport sequence loss, feedback age/freshness, relative queue delay, and current in-flight bytes. It is the only owner of acknowledged in-flight accounting: records without feedback expire after `clamp(2*SRTT + 500 ms, 750 ms, 5 s)`, decrement in-flight bytes, and increment dedicated expired-packet/byte counters without being counted as new loss or causing congestion backoff. Every accepted current-revision feedback sample has a monotonically increasing sample ID, and the congestion controller can consume that sample at most once. The pacer cannot erase estimator in-flight state to manufacture writability. This packet ring is independent of the existing 256-entry sent-frame ring used to age damaged-frame evidence; neither retains encoded payload.

`DesktopMediaSendPacer` owns one worker, one active payload, and one latest-pending payload. Its token bucket permits at most 20 ms of burst credit while always allowing at least one fragment. The in-flight limit is `clamp(rate * max(2*SRTT, 200 ms), 64 KiB, 1 MiB)` and the same budget supplies the dynamic DataChannel high/low-water contract. Binary `send=false` means accepted into the library queue, not failure. `onBufferedAmountLow` and `onAvailable` wake the pacer; there is no polling worker.

The pacing bitrate is a burst ceiling, not the encoder's average production rate. A lower encoder target therefore bounds future upward probes but does not collapse an already healthy pacing ceiling and make the next multi-fragment IDR miss its deadline. Actual bytes remain bounded by encoder output, token credit, acknowledged in-flight state, and the DataChannel watermark.

The encoder runs only when the pacer has a pending slot. Before fragment zero, the pacer computes `pacing_duration = ceil(wire_bytes / pacing_rate)` and adds the existing RTT guard `clamp(2*SRTT + 100 ms, 250 ms, 2000 ms)`. The combined whole-frame deadline is bounded to five seconds; a frame that cannot finish inside that bound is rejected before any fragment is emitted. The Controller reassembly window is 5.5 seconds, so every admitted sender deadline has a bounded delivery margin. Token, in-flight, DataChannel-buffer, and closed-channel deadline causes have separate telemetry; a local token-budget rejection is not recorded as fresh transport loss or DataChannel backpressure. Deadline expiry, send failure, or abandonment after partial send marks the prediction chain as requiring a keyframe; subsequent P frames are rejected and Host requests an IDR. The retained static capture can therefore be re-encoded into a new IDR without allocating another frame slot, and only successful paced delivery clears the request generation.

The Host congestion decision merges DataChannel backlog, RTT queue growth, and the transport estimate once per window. Receiver assembly damage is the fallback congestion signal only while application transport feedback is unavailable; with fresh transport feedback it requests dependency recovery without multiplying the same loss into another backoff. Severe buffer/queue growth backs pacing off by 30%; ordinary pressure backs off by 15%. Pure loss below 2% blocks probing but does not reduce the rate. At least three consecutive lossy windows, or loss with queue growth, is required for loss-driven backoff. Two seconds of fresh feedback, queue delay below 40 ms, and no backlog probes by `max(+8%, +100 Kbps)`, bounded by the encoder clarity budget and 20 Mbps. Pacing may remain below the encoder clarity budget: capture opportunities are skipped instead of accumulating latency or automatically reducing resolution.

`receiver_network_stats` reports assembly truth separately from GUI presentation: received fragment count, completed encoded-frame count, completed keyframes, incomplete/dependency frame drops, dropped keyframes, reassembly timeouts, the latest received/complete/complete-keyframe IDs, the observed media `rate_revision`, the observed source-activity revision, and latest displayable/displayable-keyframe IDs. `latest_presented_frame_id` is diagnostic only. A frame attempt is counted only when it completes or is definitively discarded, so a GUI-owned presentation counter cannot make every direct-pipe frame look lost. With fresh application transport feedback these facts do not own a second rate-control path.

GUI-to-runtime stdin and runtime-to-GUI stdout control frames use prefix `REDCLAW_CONTROL_V1 ` plus Base64-encoded compressed Protobuf. The complete local frame is limited to 64 KiB and occupies one line, so ordinary process logs cannot be parsed as commands. The expanded Protobuf has its own 64 KiB limit. Network DataChannels use binary without Base64. [Wire format and synchronized upgrade](compressed-protobuf-wire-v1.md).

## Viewport and encoder resolution

Controller sends only the committed playback canvas customer area in physical pixels (`logical size * devicePixelRatioF`). Host resolves the largest even output dimensions that:

1. do not exceed the capture source;
2. do not exceed the requested canvas;
3. do not exceed an explicit manual cap, when non-zero; and
4. preserve the capture aspect ratio.

An aspect mismatch remains letterboxed by the Controller renderer. A repeated request that resolves to the current dimensions acknowledges the target without rebuilding the encoder. Network adaptation never lowers this requested resolution. Hardware sessions are no longer preserved across a real dimension change merely to avoid restart: a safe backend rebuild applies the new dimensions, while backend failure falls through the existing candidate chain to software. A GPU-to-CPU readback is explicitly diagnosed when required. `stream_target_applied` is published only after a keyframe with the new dimensions has entered the two-slot pacer, so Controller scaling cannot masquerade as Host resolution application.

The initial target bitrate is approximately `width * height * fps * 0.08` bits/second and capped at 20 Mbps. That same `0.08 bit/pixel/transmitted-frame` budget is the clarity floor above 1 FPS, with an absolute 400 Kbps minimum. At 1 FPS all-IDR pacing uses `max(400 Kbps, width * height * 1.5 / 1000 Kbps)`. The independent pacer may run below this clarity budget and admit fewer latest frames; it does not build a queue or automatically reduce resolution. Encoder restarts retain a two-second minimum cooldown, while fresh incomplete-frame evidence can request dependency recovery without duplicating the transport backoff.

The Controller reassembler starts and resets in `awaiting keyframe` state. Any parse error, missing fragment, overflow, superseded incomplete frame, or timeout immediately clears the partial payload and returns to that state. While waiting, complete or partial non-keyframes are discarded once per frame because they depend on a missing reference and could produce broken decode output. A newer keyframe start supersedes any older incomplete frame. The first latest complete I frame immediately unlocks Controller delivery; Host never waits for an acknowledgement and continues normal I/P production so control RTT cannot stall the media pipeline.

Runtime-to-GUI shared memory is version 3, carries the media `frame_id`, and remains bounded with the decoder queue to two frame slots. The GUI jumps directly to the newest committed sequence; a sequence gap resets decoder dependency state and requests a fresh keyframe. The writer never overwrites the slot currently being decoded. The GUI reports a frame as displayable after decode and insertion into the latest-only playback cache, coalesced to at most one report per 250 ms except when crossing a static reference keyframe. Actual swapchain presentation is diagnostic only, so a minimized Controller cannot keep a static Host retransmitting IDRs. The GUI does not infer network loss from an idle display deadline: Desktop Duplication is event-driven, so an unchanged desktop can correctly produce no new captured frame. Only the runtime reassembler reports `playback_starvation` after positive damage evidence, such as superseding an incomplete frame or expiring an incomplete assembly. Host uses that evidence to request an IDR; the transport estimator remains the only rate owner while its feedback is fresh. At 1 FPS every selected latest frame requests IDR on the already-running encoder. This is all-intra output pacing, not a rebuild to a GOP=1 encoder.

## Static source activity

`source_activity_state` distinguishes `unknown`, `active`, `static_pending`, and `static` with its own revision, reference frame/keyframe, geometry revision, and rate revision. The first captured frame enters `active` and requests IDR. At least three healthy capture timeouts spanning 500 ms enter `static_pending`; a poll heartbeat gap over one second or a real capture failure enters `unknown` and is never classified as static.

`static_pending` encodes the retained latest capture as a reference IDR. The Host enters `static` only after a same-revision Controller report shows that reference keyframe decoded into the GUI playback cache. It then stops media encode/send while reliable control heartbeat and statistics continue, and freezes the last fresh active-network profile. Lost or discarded references retry with bounded `1/2/4/5 s` backoff only when no same-revision packet remains in flight and the two-slot pacer has room. A committed viewport change creates a new static revision, wakes the encoder worker, and sends exactly one new-size reference path. The first real source change exits pending/static, advances revision, and requires a new IDR. Thus zero media traffic in `static` is an explicit healthy source state, not a congestion or disconnection signal.

GUI presentation uses a high-priority latest-frame event instead of a normal queued invocation. Per-frame code only decodes/presents and updates frame telemetry; remote-control UI and connection-flow transitions run only on the first presented frame or an actual state change. D3D11 swapchain presentation uses non-blocking `DXGI_PRESENT_DO_NOT_WAIT`; a busy compositor drops that obsolete presentation instead of falling back to a blocking CPU path. Runtime stdout is drained in bounded line batches, and the session/runtime log views share one bounded 4096-line document so diagnostic bursts do not duplicate text layout work. Telemetry records dispatch post/coalescing/wait, handler time, D3D11 present time, and busy drops.

Host adaptation uses one network-pressure decision per window rather than multiplying independent RTT, buffer, assembly-loss, and transport-loss reductions. RTT is measured with the Host steady clock; a valid session sample is classified by `queue_delay = max(last_rtt - min_session_rtt, 0)`, not by absolute path RTT. Queue delay below 40 ms is relief, 120 ms is pressure, and 200 ms is severe pressure. This allows a stable 180 ms WAN path to retain cadence while still reacting to an additional 120-200 ms of queuing. Receiver assembly facts remain a damage/IDR signal and enter congestion only as a fail-safe while transport feedback is unavailable. The encode-budget gap is bounded by frames actually delivered by the event-driven capture source, and low source activity never lowers the target FPS: a static desktop simply produces no duplicate frames. Encoder parameter rebuilds retain the two-second cooldown and pixel-dependent clarity floor; network pressure never lowers the committed resolution.

All encoder backends use low-delay mode with `max_b_frames=0`, lookahead disabled, and an ultra-low-latency queue where supported. Runtime rejects any profile that enables B frames or lookahead. Every encoder open/reopen forces its first frame to I; QuickSync additionally enables `forced_idr=1` so a requested recovery frame is an independently decodable IDR instead of a non-IDR I picture. Resolution changes retain the encoder's nominal FPS/GOP configuration and observe the same two-second cooldown; pacing and encoder configuration are separate state. `EncoderExecutionSession::update_rate_control()` capability-probes `AV_CODEC_CAP_PARAM_CHANGE` and updates bitrate/maxrate in place when supported. FPS changes are applied by Host pacing without reopening the encoder. Unsupported or failed hot updates retain the existing cooldown-governed restart fallback for rate-control changes; 1 FPS per-frame IDR pacing never rebuilds an alternate encoder.

On Windows, a D3D11 Video Processor attempts native-texture scaling before hardware-frame encode. If Video Processor or FFmpeg hardware-device derivation is unavailable, CPU readback plus the existing libswscale path is used. Runtime diagnostics distinguish `encoder_gpu_scale`, `encoder_gpu_to_cpu_readback`, input mode, capture adapter, and the exact hardware-frame block reason; fallback is never called zero-copy.

## Windows geometry transactions

`PlaybackWindowGeometryController` installs a Qt native event filter on the playback top-level HWND and sets `DWMWA_TRANSITIONS_FORCEDISABLED` for that window only.

- `WM_ENTERSIZEMOVE` snapshots the committed window rectangle.
- `WM_SIZING` updates a no-focus/mouse-transparent outline and restores the proposed real rectangle to the committed value.
- `WM_WINDOWPOSCHANGING` freezes the real HWND while an interactive size transaction is active.
- `WM_EXITSIZEMOVE` applies one final `SetWindowPos`, then queues one viewport commit.
- Escape, `WM_CANCELMODE`, capture transfer, or capture loss while the mouse button remains down cancel without committing.
- Maximize, restore, taskbar actions, title-bar double-click, DPI change, and Snap coalesce native size notifications into one queued final commit.
- Minimize clears the pending state and never publishes `0x0`.

`D3D11PlaybackCanvas` suppresses swapchain resizing while a transaction is active and deduplicates final physical sizes by transaction ID. Telemetry joins `geometry_preview_total`, `geometry_commit_total`, `swapchain_resize_total`, `viewport_request_total`, and `encoder_resolution_reconfigure_total`.

## Remote diagnostics

The process logger keeps a redacted ring of at most 4096 lines or 1 MiB. It never reads a caller-supplied remote file path. Snapshot requests are limited to 2000 lines or 512 KiB. Log chunks are at most 8 KiB; follow backlog is at most 256 KiB and reports `gap=true` after eviction. Only one queued log chunk is sent per runtime iteration and direct control commands are processed first.

Debug builds allow remote diagnostics by default. Release builds deny them unless the local endpoint explicitly enables `--allow-remote-diagnostics`. Text is redacted before entering/sending the ring and again before Controller display. Remote-log protocol self-output is filtered to avoid reflection loops.

When the GUI is started with local debug control enabled, `remote_log_snapshot` sends one snapshot request over `redclaw-control-v1`, and `remote_log_read` returns the bounded, already-redacted result through the current-user Named Pipe. The same local-only interface exposes typed `remote_control_start`, `remote_control_pause`, `remote_input_mouse_click`, and `remote_input_key_press` QA actions. They use the normal consent, frame-ready, foreground-keyboard, bounded-queue, epoch, and acknowledgement paths; only paired click/key actions are accepted, and no key value is written to logs. These commands accept no file path, arbitrary command, free-form text, or runtime argument. Stream capture writes to the process logger even when the original stdout/stderr handle is unavailable, so a background Debug Host still populates both its file log and memory ring.

## Recovery

Loss of either required DataChannel, or ICE disconnected/failed for at least three seconds, invalidates the connected session. Host clears stream state and returns to generation-zero waiting. Controller publishes a fresh request using the same machine code after 1, 2, 4, 8, and 16 seconds, for five attempts. These values are retry start delays, not complete DHT/ICE attempt lifetimes: after each publication the current attempt remains in flight for up to 90 seconds so rendezvous propagation and ICE convergence cannot consume all five retries prematurely.

Every rebuilt session has a new epoch and fresh peer/channel generations. Encoder/decoder state, video reassembly, pacer active/pending payloads, transport sequence, sent-packet metadata, feedback windows, congestion probe/backoff state, GUI direct-frame pending data, remote-log queues, and control guards are reset. Old worker callbacks are generation-rejected. After a successful reconnect, Controller resends the last committed viewport and active log-follow request; hello/capabilities and network statistics resume normally. A user-requested process stop exits instead of reconnecting. Exhaustion leaves the GUI available for manual retry.

Debug QA may close exactly one selected required channel with `--stream-qa-force-required-channel-close media|control` after both channels have remained ready for five seconds and at least one complete keyframe has reached the GUI pipe. It uses the same typed channel-close path as a real loss and is never enabled implicitly. `required_channels_open_total`, `recovery_reset_total`, `recovery_success_total`, `qa_forced_channel_close_frame_id`, `qa_post_reconnect_frame_id`, and `qa_post_reconnect_direct_pipe_written_total` prove that the new epoch reopened both channels and delivered frames newer than the forced-close boundary. Remote input remains paused with an empty Host queue; reconnect never silently restores Controller capture.

## Validation boundary

Release validation requires standard Debug and Release builds plus focused protocol, network, capture, service, diagnostics, and UI tests. A local public-DHT dual-GUI run must prove real capture, encode, transport, reassembly, decode, successful new-frame presentation, and visible playback with no decode or presentation failure.

The media channel keeps ordered zero-retransmit delivery. Reassembly failures request bounded keyframe recovery without allowing obsolete P-frames to accumulate. Control message IDs are assigned in actual send order, and reliable control acknowledgements never block the media callback.

Health and recovery telemetry is endpoint-owned. Loss of either required DataChannel must create a fresh session epoch, clear stale codec/queue/feedback state, reopen both required channels, and deliver a frame newer than the forced-close boundary. Remote input remains paused until the Controller explicitly restores it.

Windows capture uses the fallback order `Desktop Duplication -> Windows Graphics Capture -> GDI`. WGC frame-pool recreation must release old-size frames, advance geometry once, keep queues bounded, and retry the current capture only with a new-size frame. Native encoder input may fall back only when the selected backend rejects the surface, and that fallback must be visible in diagnostics.

Incomplete-frame feedback is classified as fresh, delayed, expired, stale-revision, or unknown. Only fresh, revision-matched evidence may immediately change the sending rate. Delayed evidence is queued once; expired or stale evidence is recorded without changing rate. FPS reduction uses a saturating floor and cannot wrap to a higher rate.

Generated reports, screenshots, packet samples, hashes, hardware identities, and cross-machine observations belong under ignored evidence directories. Physical two-machine recovery, live display-mode changes, additional GPU backends, and broader TURN/NAT combinations remain separate validation work.