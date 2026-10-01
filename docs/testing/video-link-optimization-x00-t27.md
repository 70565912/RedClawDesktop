# X00-T27 video link optimization

The task ledger is [MODULE_KANBAN](../runtime/MODULE_KANBAN.md). Implementation,
local validation and cross-LAN performance confirmation are separate milestones.
Keep real capture, native capture dimensions, codec quality and the current media
and Control wire formats. The old remote Host's reported 9.8 ms readback and the
natural 1.58 FPS scene are reference observations, not the new-role baseline.

## Accepted optimization scope (2026-10-01)

**X00-T27 is accepted under the operator's revised criterion:** confirm that the
optimization is implemented and accept a clear resource/cost benefit established
by source analysis. Existing functional/build and measured stage evidence is
retained. A new matched historical-Client run or an isolated empirical percentage
for every phase is no longer a completion gate. This changes the acceptance
decision, not the measurements or their provenance. The earlier real-Controller
P1 P95/P99 target and isolated P2 latency gain remain unmeasured/unconfirmed.

The audited repository revision is `58002bb`; relevant production files have no
working-tree source edits, and file SHA256 values are retained locally under
`t27-static-acceptance-20261001-01`. This is a source audit, not an assertion that
an old running Client has that commit. The prior local GUI pair is identified
independently by main-app hash12 `47087D9C0484`.

| Phase | Confirmed optimization or completed evidence | Accepted basis |
| --- | --- | --- |
| B0 | Deterministic fixed-seed scene and bounded observation/trace tools; retained same-Host baselines | Existing reproducible recordings, not a new performance claim |
| P1 | One bounded text cache; independent documents; visible views batch at 100 ms, at most 64 lines/32 KiB; hidden views stop the timer and return before document work | Source-confirmed removal of per-record document mutation and hidden-view refresh; prior functional validation retained |
| P2 | Desktop profile fixes nominal FPS to 30, time base to 1/30 and GOP to 60; initial budget uses the same nominal cadence; submission pacing stays independent | Source-confirmed removal of low-cadence clock/GOP/budget coupling, plus existing software/QSV and real GUI resize/recovery checks |
| P3 | Three leased CPU slots retain image storage; native-texture slots reuse idle handles; consumers retain strong references and latest pending semantics | Source confirms bounded reuse; retained same-Host capture-copy/CPU reduction supports it |
| P4 | Main frame is published and encoder notified before thumbnail preparation; JPEG/serialization/send run in one bounded worker with one replaceable pending image | Source confirms removal of JPEG/send from the capture publication path; retained publication-stage reduction supports it |
| P5 | Successful hardware output confirms GPU-only capture delivery; conversion/map stays on GPU, with device/generation ownership and CPU fallback | Source confirms the main-video CPU readback/copy path is removed after confirmation; retained NVIDIA/Intel stage measurements support it |
| P6 | Bounded memory trace with explicit overflow and export after completion; retained accounting of key/ordinary frames and exclusive waits | Attribution/tooling complete; P6 itself is not claimed to reduce latency |

### P1 source-derived benefit

Before `a8ec6e4`, the GUI calls `QPlainTextEdit::appendPlainText` for every incoming
record and the mirror shares the same document. The current
[buffer/view implementation](../../src/ui/runtime_log_view.cpp) stores text once;
[GUI integration](../../src/ui/gui_shell.cpp) shares the buffer rather than the
document. `schedule_refresh` arms one single-shot 100 ms timer only for visible
views; `hideEvent` stops it and `flush_pending` returns before document work when
hidden. The 4096-line/4 MiB cache and 64-line/32 KiB batch also bound input retention
and one refresh's insertion work. File logging remains independent.

For the existing [80-lines/s replay](../../src/ui/runtime_log_replay.h), each
record invokes append individually. With steady visibility and no explicit
flush/clear calls, the old path performs about 80 document appends per second;
the current path performs at most about 10 refresh appends per second per visible
view, while a hidden view performs none. This is a source-derived count of
document mutations/refreshes, not a measured CPU or P95/P99 improvement. Separate
documents can use more view storage when both views are visible; the shared text
cache does not eliminate every display allocation. The clear cost reduction is
sufficient for P1 acceptance under the new criterion.

### P2 source-derived benefit

Before `c9df422`, the main runtime builds its codec profile from current/applied
submission FPS. The ordinary profile sets clock FPS to that request and desktop
GOP to twice that FPS, with a minimum of 2. A codec initialized at 1 FPS can
therefore retain a 1/1 clock and a 2-frame GOP when motion resumes. The current
[desktop profile](../../src/capture/src/capture_module.cpp) explicitly normalizes
to 30 FPS before deriving clock, GOP and initialization bitrate. Its FFmpeg
context uses that profile directly. This eliminates the low-cadence 2-frame-GOP
configuration and under-sized startup/resize budget, reducing avoidable periodic
keyframe pressure on motion resumption. Forced recovery IDRs remain available;
no actual keyframe-count, bitrate, CPU or latency reduction percentage is inferred.

The [runtime](../../src/main.cpp) applies actual target FPS to submission cadence;
FPS alone does not satisfy the codec bitrate/restart predicate. The PTS assignment
is the maximum of the next tick and elapsed monotonic time rescaled to the codec
clock. Existing isolated software/QSV checks explicitly cover requested 1/5/30
FPS, geometry rebuild and monotonically increasing PTS; nine real Intel GUI
windows and one DHT recovery supply independent media-path evidence. A live
adaptive target 1→30→1 was not observed and is not relabeled as performed. The
implemented coupling removal and existing functional evidence are sufficient
for P2 acceptance under the new criterion.

P3/P4/P5 retain their actual scoped measurements, including P4's differing
network identities and P5's higher GPU memory/approximately 21 FPS scene rate.
No end-to-end zero-copy, sustained 30 FPS or universal latency benefit is added.
The historical five-minute NVIDIA continuity/visual result remains within its
original machine/role scope; no new Intel five-minute run is claimed. Historical
Client diagnostics were not restored in this closure. X00-T30/X00-T31 remain
separate incident/attribution records. Their [later bounded local/Linux review](video-link-linux-checks-20261001.md)
temporarily closes them by explicit operator instruction; historical causes remain
unknown and no incident resolution is inferred from T27 acceptance.

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

The operator briefly selected local Client / remote Host on 2026-09-29; its
[nine-window baseline](video-link-remote-host-baseline-20260929.md) remains a
separate historical recording. The later P5 acceptance restored **local controlled
Debug Host / remote Client**. Retain that current topology and the network
configuration while finishing T27/T30. Deployment and remaining results follow
the current [task ledger](../runtime/MODULE_KANBAN.md), not the historical role
selection. Do not transfer a performance conclusion between the two machines.

The [P5 report](video-gpu-input-p5-20260929.md) records the completed local NVIDIA
comparison and continuity checks, the peer's Intel functional gate, and the
completed independent Intel performance comparison. The later
[local Intel P2 checks](video-link-baseline-20260928.md#local-intel-p2-verification-2026-10-01)
record nine real source-cadence windows, nominal-codec/geometry checks, explicit
QSV low-requested-FPS cases and one DHT recovery using an unchanged temporary
local Host/Client pair. Live adaptive target 1→30→1 and isolated P2 gain remain
unqualified. P1's old diagnostic refusal is endpoint-specific; new owned local
endpoints work, while its matched real-Controller log benefit is still unmeasured.
P6 reuses same-Host records
for [offline attribution](video-send-attribution-20260929.md). The recovered
[P4 publication audit](video-link-baseline-20260928.md#p4-publication-stage-audit-from-retained-cpu-recordings)
establishes that stage's improvement with its network-matching limit. P1's real
Controller tail-latency target remains unqualified; independent local replay is
not a replacement for it.

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

X00-T29 supersedes P2's desktop bitrate calculation after the observed low-FPS
resize defect: the encoded dimensions and nominal 30 FPS set the codec budget;
submission cadence and the network pacer remain independent. See the
[repair and regression evidence](video-gpu-input-p5-20260929.md#low-cadence-resize-codec-budget-repair-x00-t29-2026-09-30).

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

The current completion decision follows [the revised operator criterion](#accepted-optimization-scope-2026-10-01)
above: implementation, a clear source-proven cost/resource benefit, and retained
functional evidence qualify the optimization. X00-T27 is done on that basis.
Static benefits must remain labeled static; source provenance must not replace
running-binary identity, and previous measured outcomes must not be rewritten.

The original empirical target was at least 20% lower real-video P1 dispatch
P95/P99 under fixed logs, with matching measured allocation/copy/time gains for
other phases, plus five-minute dynamic continuity/visual evidence. Those
historical targets and measurements remain useful for future evaluation but are
not outstanding T27 closure gates under the new instruction. Overlapping ranges
remain "improvement not confirmed"; local replay does not measure remote Client
benefit. Run only affected tests when requested; main builds remain serialized
through `build.ps1`. No new build or load is necessary for this documentation/source
acceptance decision.
