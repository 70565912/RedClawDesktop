# X00-T27 video link optimization

The task ledger is [MODULE_KANBAN](../runtime/MODULE_KANBAN.md). Implementation,
local validation and cross-LAN performance confirmation are separate milestones.
Keep real capture, native capture dimensions, codec quality and the current media
and Control wire formats. The old remote Host's reported 9.8 ms readback and the
natural 1.58 FPS scene are reference observations, not the new-role baseline.

## Execution sequence

For the original local-Host deployment workflow: finish local checks, commit
and push the relevant source and required dependencies, then use `build.ps1
-Configuration Debug` to build/publish that pushed source. Bring up the local
controlled Debug Host with its existing credential. Record DHT publication and
waiting readiness, then wait for the operator's remote Client/Controller and real
media before starting load or sampling. Prepare a complete rollback artifact and
a session-independent recovery task before replacing a serving Host. Preserve the
connected-Host exit confirmation. Older capable Clients may connect; identical
versions are not a gate. Controller log benefits require a Client containing P1.

The operator subsequently selected local Client / remote Host on 2026-09-29.
Use that running topology for the new-role baseline; do not automatically switch
roles or deploy a remote Host. See the current [task ledger](../runtime/MODULE_KANBAN.md).
P6 can independently reuse earlier same-Host recordings for
[offline attribution](video-send-attribution-20260929.md); those results do not
qualify the current remote Host.

## Phases

| Phase | Scope | Evidence |
| --- | --- | --- |
| B0 | Fixed seed, absolute frame trajectories and frame index, real desktop scene; static, dynamic and dynamic plus fixed logs | 10 s warmup, 60 s measurement, three repeats; program identity, effective codec configuration, frame trace, CPU, memory and queues |
| P1 | Shared bounded text cache; separate documents; hidden views stop layout | 4096 lines / 4 MiB UTF-16 text; visible refresh every 100 ms, at most 64 lines / 32 KiB per batch; copy cache, clear pending, retain history position |
| P2 | Nominal encoder 30 FPS, time base 1/30, GOP 60 frames; adaptive submission remains 1–30 FPS | 1→30→1, resize at low FPS, reconnect, monotonic PTS, no filler frames or transient-FPS rebuilds; retain bitrate adaptation and recovery IDR |
| P3 | Three leased CPU slots and latest pending frame; separate adapter identity from native textures | Bounded allocation/copy counts, pool exhaustion, old-generation references, resize, cursor and crop; CPU/GPU/initial hybrid delivery |
| P4 | Publish main frame before preparing a maximum-edge-320 thumbnail; separate JPEG/send worker | One active and one replaceable pending task; reuse WIC; 1 Hz and geometry refresh; discard stale generations and stop safely |
| P5 | Shared device lifetime and recursive context synchronization; D3D11 multithread protection and FFmpeg callbacks | QSV NV12 and declared formats, real successful encode before GPU-only switch, next-real-frame CPU fallback plus IDR; actual readback/preparation counters, recovery and reconnect |
| P6 | Existing bounded frame trace separates I/P token, in-flight, buffered, send-call, overshoot and frame age | Recomputable attribution; compare keyframe sizes/frequency before/after P2; do not tune bitrate, bursts, congestion, priority or queue capacity speculatively |

GPU thumbnail preparation must downscale before readback. Context locks must not
span frame waits, networking or thread joins. Device/synchronization ownership
outlives texture/encoder references. New components own their stop order. Local
trace extensions need a version, frame ID, capture generation and denominators;
sampling off must not write each frame. Borrow Radar's allocation/ownership ideas,
not FIFO behavior; compressed frame dependencies and IDR recovery remain intact.

## B0 tools and contract

`scripts/capture/run-local-high-motion-scene.ps1` uses seed 2700 by default and
absolute reflected trajectories. Frame N has the same pixels for a fixed seed,
geometry and scene FPS, independent of late/skipped timer callbacks. Static mode
does not update the HUD or invalidate the window on each tick. `-VerifyOnly`
renders frame 0, frame 120 and frame 0 again offscreen, verifying repeatability
without introducing a real desktop load. It does not prove capture/codec delivery.

`scripts/capture/run-video-link-baseline.ps1` accepts a controlled Host run directory,
a contract JSON and a new evidence directory. `-PrepareOnly` validates the contract
and saves the schedule without opening a scene or sampling. Execution requires a
running Debug DHT Host, a connected Client, real captured/sent frames, zero synthetic
frames, matching program hash and matching log visibility. The host's existing
frame-trace sidecar and GUI trace are reused. The scene is fullscreen and topmost
so a static trial cannot capture changing windows outside a clipped client area;
the covered Host log retains its existing visible/layout state. Scene reports
record the actual client geometry (which can differ from physical capture pixels
under DPI scaling). The fixture invokes no remote command.

Example contract (replace all geometry and configuration values with measured
values for the selected local Host / remote viewport before a real run):

```json
{
  "capture_width": 1920, "capture_height": 1080,
  "encode_width": 1920, "encode_height": 1080,
  "viewport_width": 1920, "viewport_height": 1080,
  "log_visible": true, "mirror_visible": false,
  "network_configuration_id": "operator-recorded-configuration-digest",
  "diagnostic_interval_ms": 10000
}
```

The contract records source/viewport/network settings; the collector verifies
encoded geometry against every retained trace row and records actual source/codec
diagnostic summaries. The operator/endpoint must verify the declared capture and
remote viewport before comparing runs. No script silently resizes the capture or
changes network settings. `DynamicLog` explicitly starts a bounded local Debug
fixture: 80 deterministic lines/s with at most 16 lines of catchup per callback,
using the normal file sink and UI cache. It stops explicitly after measurement and
has a 90 s safety limit. Local Debug IPC actions `log_replay_start` and
`log_replay_stop` do not change the peer protocol. Release builds reject them.

After the 10 s warmup, the collector waits for the Host's next diagnostic tick to
consume the trace request, then measures 60 s. `host-arm.json` records this extra
0–10 s alignment barrier and the Host trace clock; GUI/status boundaries are
recorded separately. The scene remains open through the recorder's drain period.
A static window may contain zero newly encoded frames: retain its zero rate and
CPU/memory evidence, report frame quantiles as unavailable, and verify retained
source/encoded geometry from runtime stats. Do not inject filler frames. Connection
readiness uses the live connection/channel and real-media counters; the transient
`connected` phase between runtime state and stream-stat records is not a disconnect.

Reports are local, may contain endpoint paths/network metadata and must not be
committed. CPU samples retain cumulative process seconds and memory bytes; compute
deltas over each recorded interval. GUI and Host traces retain their own monotonic
boundaries. Never subtract uncalibrated timestamps from different machines.

## P1 local comparison

The developer-selected `RuntimeLogView.DiagnosticPaintReplay` is excluded from
ordinary CTest. `REDCLAW_QA_LOG_PAINT_SOURCE` selects redacted fixed text;
`REDCLAW_QA_LOG_PAINT_LEGACY=1` replays the old shared-document append path in the
same test binary. `REDCLAW_QA_LOG_PAINT_SECONDS=60` and
`REDCLAW_QA_LOG_PAINT_WARMUP_MS=10000` select the matched window. Repeat each mode
three times. A bounded 30 Hz synthetic notification measures local GUI dispatch
wait, not video capture, decoder output or end-to-end latency. Report this scope
separately from real-media GUI traces. Functional cases cover hidden/dual views,
line/byte eviction, long lines and surrogate boundaries, copy/clear, empty records
and scroll following. The existing complete file sink remains outside UI eviction.

## Acceptance

Compare three-run medians and ranges. P1 targets at least 20% lower real-video
dispatch P95/P99 under fixed logs. Other phases must reduce the matching measured
allocation/copy/time metric. Overlapping baseline variation is "improvement not
confirmed". Local replays cannot qualify remote Client GUI benefit. Finish with
a 5-minute real dynamic run, frame age/memory/failure/recovery counters and decoded
text, thin lines, color patches and cursor comparisons. Run only affected tests;
main builds are serialized through `build.ps1`. Keep P2–P6 planned until the B0/P1
checkpoint supplies the appropriate baseline.
