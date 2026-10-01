# X00-T27 local Host baseline, 2026-09-28

The later [2026-10-01 source-based acceptance](video-link-optimization-x00-t27.md#accepted-optimization-scope-2026-10-01)
closes T27 under the operator's revised criterion. The measurements and their
limitations below remain unchanged; source acceptance does not supply missing
real-Controller P1 or isolated P2 latency percentages.

## Local Intel P2 verification, 2026-10-01

The operator authorizes a temporary local Host/Client pair for P2 verification.
This recording is separate from the historical NVIDIA/cross-machine baselines
below and from the independent Intel P5 CPU/GPU comparison. Both temporary GUI
instances reuse main-app hash12 `47087D9C0484`, with the full binary hash retained
locally; its source commit is not proved. Credentials and UI settings are isolated
test fixtures. No main-app build/publication, display-mode change, permission
change or user-settings modification is performed.

Three low/high/low source sequences complete nine valid windows, each with 10 s
warmup and at least 60 s measurement. The source uses seed 2700 and target cadence
1/30/1 FPS. Actual dynamic redraw is 21.19–21.29 FPS, so this is not sustained
30 FPS delivery evidence. Real DDA capture stays 1680×1050; synthetic counts and
capture/encode/send/GUI-decode/GUI-presentation failure increments are zero in
all valid windows. D3D11 decoded surfaces provide the actual GUI playback path;
runtime-only decoded/rendered counters are not its progress counters.

| Source stage | Seconds, median [min, max] | Capture delta | Encode/send delta | GUI decode delta |
| --- | --- | --- | --- | --- |
| Low before motion | 61.209 [61.075, 61.476] | 64 [64, 136] | 64 [64, 134] | 64 [64, 118] |
| Motion | 61.063 [60.791, 61.152] | 1280 [1275, 1337] | 414 [406, 1269] | 447 [408, 1277] |
| Low after motion | 61.221 [60.327, 61.567] | 65 [65, 294] | 65 [64, 215] | 73 [65, 220] |

These are three-run medians/ranges of cached-status counter deltas. Cache refresh
boundaries differ between stages/endpoints; they do not form exact matched-frame
accounting, latency distributions or an isolated performance comparison. Source
cadence is distinct from the live adaptive submission target, observed at 30 and
18 FPS. An initial preparation run used the wrong runtime decode counter and had
zero measured windows. A later scene exited at approximately 63 s before its
75 s safety deadline; its partial window is invalid and its close cause remains
unknown. The completed first trial is retained, and only the missing two trials
are subsequently measured with a 95 s safety duration. Raw attempts are retained.

Across 97 active QSV diagnostic rows, nominal FPS is 30, time base is 1/30 and GOP
is 60, with no sampled PTS regression or codec-contract deviation. Initial actual
encoding is 1064×664, even after setting the temporary output buffer to 1584×990.
Owned low-cadence viewport state changes later produce actual encoded geometries
1064×664 → 1552×970 → 1584×990, with continued GUI decoding and zero observed
decode/presentation failures. The final 1584×990 NV12 visible image uses a
1584×992 padded texture. Do not equate output-buffer geometry with encoded geometry
or transfer these temporary Client sizes to the existing remote Client.

An isolated test-only build adds a QSV invocation of the existing desktop-budget
regression helper to the local `d2930f0` snapshot. Main production source and
binary are unchanged. The guarded test build exits 0; explicit software and
QSV cases both pass (2/2). Each case encodes/decodes 20 frames across four sessions
at requested 30/5/1/5 FPS and 1184×666/1778×1000 sizes, checking nominal 30 FPS,
1/30 time base, GOP 60, geometry-derived bitrate budget, no hot rate update,
per-frame increasing PTS and decoded dimensions. This is codec-fixture evidence;
the nine GUI windows independently establish real desktop media. Test hash12 is
`F084AD2C4038`. The first guarded build did not launch because its Windows
PowerShell child was invoked from Core; the repository-standard Windows
PowerShell launcher resolves that preparation error without persistent policy
changes.

File-signaling manual reconnect fails: the temporary Client applies an offer but
ICE fails, while the Host restarts into waiting state. This is a real unsuccessful
diagnostic attempt, not an encoder-failure result. The normal local DHT/ICE
required-media-channel recovery check is recorded separately. No same-machine
pre-P2 binary comparison or frame trace is collected here; isolated P2 gain,
frame percentiles and trace overflow remain unqualified. GUI defaults request
UPnP discovery despite omission of the explicit enabling flag; the file runs
report no IGD and no successful mapping. Do not label discovery as disabled.

The subsequent DHT/ICE check confirms one automatic media-channel recovery in
the same Host/Client runtime instances. The bounded observation lasts 189.811 s;
both required-channel open totals reach 2 and recovery-success totals reach 1.
The forced-close frame is 5 and the first post-reconnect frame is 7; 17 subsequent
frames reach the GUI pipe. Final GUI decode/presentation totals are 16/16, with
zero observed media/GUI failures. Two recovered diagnostic rows demonstrate
10 s of continued progression on each endpoint. These same-machine timestamps
are not used to infer cross-machine latency. All owned processes and the source
scene stop; the main binary hash is unchanged. The raw sampler reports a timeout
because QA status omits the required-channel/forced-close/recovery fields used
by its predicate. Retained primary runtime diagnostics and GUI samples prove
recovery, and the separate reproducible log analysis records `recovery_passed=true`.
Keep this instrumentation false negative distinct from the earlier unsuccessful
file-signaling manual reconnect.

**Conclusion:** local Intel P2 nominal-codec, low-cadence codec initialization/
rebuild, real low/high/low media, viewport resizing and one DHT recovery checks
pass. A live adaptive submission target 1→30→1 is not proved: actual source
cadence changes, while runtime target diagnostics show 30/18 FPS. The isolated
codec cases cover requested 1/5/30 FPS explicitly. This observation alone does not
establish matched same-machine before/after P2 performance gain. The subsequent
source-based T27 closure accepts the implemented cost reduction under the new
operator criterion, without changing these empirical limitations.

Opaque evidence: `t27-p2-local-20261001-01` (preparation),
`t27-p2-local-20261001-02` (first valid trial plus invalid partial),
`t27-p2-local-20261001-03` (remaining trials, geometry, codec tests and analysis),
`t27-p2-local-20261001-04` (bounded DHT recovery). Raw identities, settings,
credentials and logs remain local.

### P1 historical access scope

The old Client's Debug Control status action was refused with access denied;
the historical remote execution context had no normal approval path. That blocks
its live log injection/GUI sampling. The retained record does not distinguish
execution-context restriction from pipe access rules, and does not establish a
missing password or encoder error. Its explicitly prohibited ACL/elevation/
identity/channel workarounds are not attempted. Fresh owned local P2 Debug Control
endpoints work with normal platform approval. P1 is therefore not globally
unavailable; its real-Controller matched 80-lines/s before/after tail benefit
remains unmeasured and is not supplied by these P2 runs.

## Historical B0 recording (2026-09-28)

Nine real-media windows completed after the operator confirmed the remote Client
display and fixed its viewport. Host source: `808c144`; collector source: `8da3530`.
The controlled Host remained connected, with no synthetic frames or capture,
encode or transmit failure increments. Every Host/GUI trace reported zero overflow;
all 7,966 retained encoded frames completed sending. This is a baseline, not a
before/after optimization result or an end-to-end latency measurement.

## Fixed conditions and limits

- Desktop Duplication capture: 1920×1080 physical pixels; viewport request:
  1920×1001; encoded frames: 1778×1000. `stream_video_max_width=0`; the existing
  viewport fit remains active. No capture or codec dimensions were reduced for
  this test. The fullscreen scene has 1536×864 logical pixels at the current DPI.
- Fixed seed 2700 and absolute-frame trajectories; fullscreen/topmost. Main log
  logically visible, mirror hidden; diagnostics every 10 s; network configuration
  digest and runtime identity retained in local evidence. The covered Host log
  still performs layout; its paint behavior is not the remote Controller's UI.
- Each trial warms for 10 s, then waits for the diagnostic tick to arm the Host
  trace. The extra alignment barrier is recorded. Host traces are exactly 60 s;
  separately bounded status/CPU snapshots span approximately 60–61 s.
- The scene requests 30 FPS but actually renders about 21.3 FPS on this machine.
  Dynamic results are therefore a repeatable source-limited workload, not proof
  of maximum link throughput. Static source painting stops after one paint, but
  captured/sent activity varies; do not attribute that variation to an encoder
  capacity limit or assume its cause is established.
- The windowed/clipped attempt and the attempt interrupted by a transient
  `connected` status are retained and excluded. The collector now verifies live
  channels and real-media counters rather than one status label; unchanged
  static content may legitimately have no new encoded frame.

## Three-run median and range

CPU is percent of one logical core: 100% means one full core. Frame age below is
`last_send_us - capture_ready_us` on the Host's monotonic clock. It excludes the
capture/readback before publication and all remote receive/decode/display time.

| Metric | Static | Dynamic | Dynamic + 80 log lines/s |
| --- | --- | --- | --- |
| Sent FPS | 1.52 [0, 3.43] | 21.30 [21.28, 21.32] | 21.30 [21.30, 21.32] |
| Host process CPU, one core % | 11.91 [6.89, 18.56] | 59.92 [59.31, 61.16] | 61.02 [60.83, 62.41] |
| GUI process CPU, one core % | 11.83 [11.76, 12.73] | 9.61 [9.57, 9.81] | 18.65 [17.31, 19.14] |
| Encode P95, ms | unavailable in zero-frame trial | 27.48 [26.06, 27.62] | 26.03 [25.76, 29.93] |
| Host frame-age P95, ms | unavailable in zero-frame trial | 45.66 [45.21, 46.04] | 46.15 [45.59, 46.67] |

The 0.49 ms frame-age P95 difference between the two dynamic scenarios is inside
the dynamic baseline's 0.83 ms spread: a tail-latency effect is not confirmed.
The hidden mirror refreshed zero times in every window. Fixed replay generated
about 4,800 lines per measured dynamic-log window. These are Host observations;
there is no remote Controller P1 dispatch P95/P99 comparison yet.

## Input-path and sending observations

Dynamic capture-copy weighted means are 9.62–9.69 ms per successful capture, and
encoder input preparation is another 11.16–11.31 ms per encode attempt. These
support investigating P3/P5; they are not additive guaranteed savings. Supporting
10 s diagnostic aggregates overlap snapshot boundaries; raw frame traces retain
their exact boundaries and denominators.

For ordinary dynamic frames, mean token waits are 7.31–7.63 ms, mean send calls
0.89 ms, and in-flight/buffered waits are zero in all three windows. Wake overshoot
averages 4.69–5.12 ms and is already part of the recorded waits, so it must not be
added again. Keep the P6 before/after attribution task; no rate, burst, congestion,
queue capacity or thread-priority adjustment follows from this baseline alone.

The baseline encoder is already configured at 30 FPS, with a 1.64–1.72% keyframe
ratio in dynamic trials. P2 also needs low-cadence initialization/resize tests;
this normal-cadence baseline alone cannot demonstrate that fix's performance gain.

## Evidence and next gate

Local generated evidence is under `build/reports/x00-t27/b0-aligned-20260928/`:
`result.json`, `analysis.json`, per-window Host CSV, GUI binary/queue traces and
summaries, CPU/memory/status snapshots, scene reports and codec/stage diagnostics.
`scripts/capture/analyze-video-link-baseline.py <report-directory>` recomputes
nearest-rank percentiles and three-run summaries. The v2 analysis corrects the
capture-copy weight to successful captures (attempts minus timeouts for these
failure-free v1 logs). The original analysis is retained locally. Raw reports
remain local because they contain endpoint data.

## P2 comparison: functional pass, latency improvement unconfirmed

The operator confirmed the remote Client display on the pushed Debug Host
`c9df422`. Nine further windows completed with the same scene, geometry, log
visibility and network configuration digest. All 9,005 traced frames were sent;
capture/encode/transmit failure increments, Host/GUI overflows and hidden mirror
refreshes were zero. GUI trace parsers accepted all nine windows. Codec diagnostics
retain nominal 30 FPS, time base 1/30 and 60-frame GOP; these windows do not replace
the local 1→30→1, low-cadence resize/restart and real decode checks.

Three-run medians [minimum, maximum], measured on the local Host:

| Metric | B0 dynamic | P2 dynamic | B0 dynamic + logs | P2 dynamic + logs |
| --- | --- | --- | --- | --- |
| Sent FPS | 21.30 [21.28, 21.32] | 21.32 [21.30, 21.33] | 21.30 [21.30, 21.32] | 21.28 [21.25, 21.28] |
| Host CPU, one core % | 59.92 [59.31, 61.16] | 61.90 [61.44, 62.87] | 61.02 [60.83, 62.41] | 61.41 [60.10, 61.56] |
| Encode P95, ms | 27.48 [26.06, 27.62] | 29.90 [27.06, 30.83] | 26.03 [25.76, 29.93] | 27.11 [25.44, 27.43] |
| Frame age P95, ms | 45.66 [45.21, 46.04] | 49.24 [48.96, 49.63] | 46.15 [45.59, 46.67] | 48.24 [47.48, 50.58] |
| Frame age P99, ms | 58.62 [57.40, 59.05] | 63.85 [60.51, 65.27] | 56.55 [53.14, 57.05] | 61.47 [58.35, 67.51] |
| Mean pacer time, ms | 8.72 [8.50, 8.79] | 10.30 [10.16, 10.40] | 8.73 [8.69, 8.85] | 12.02 [10.43, 12.19] |

The observed tail is higher than B0, not an improvement. Although network settings
were unchanged, adaptive pacing differed: B0 stayed at 16,030 kbps; P2 dynamic
stayed at 12,434 kbps and its final two log windows at 10,389 kbps. Ordinary-frame
sizes stayed around 17.5–17.7 kB; mean token wait rose from 7.31–7.64 ms to
8.92–11.02 ms. In-flight and buffered waits remained zero, and mean send-call time
stayed 0.86–0.94 ms. Wake overshoot is included in token wait, not a separate
additive delay. This identifies a measured sending contribution; it does not
establish that the P2 code caused the pacing-rate change or all tail movement.
Dynamic keyframe ratios were 1.64–1.80%, with mean keyframe sizes 31.0–31.8 kB.
Do not change rate, burst allowance, queue limits or scheduling on this evidence.

Static windows painted the scene once, but P2 captured 1,339 / 0 / 0 frames
versus B0's 206 / 0 / 91. The first-window activity is unexplained; static results
are retained and are unsuitable for a performance claim or maximum-FPS estimate.
All frame ages still exclude capture/readback, network transit and peer playback.

Evidence: `build/reports/x00-t27/p2-comparison-20260928/`. Two collector copies
ended mid-row because the CSV became visible before the Host finished writing.
Their complete original Host files survived; start-clock identity and all nine
original/copy pairs were checked. Truncated copies and hash/size recovery receipts
are preserved, and statistics use the complete originals. B0 copies all matched
their originals. The collector now waits for writer closure, validates every
numeric field and reads its own saved copy; newer Hosts publish CSV by rename
after close. This repairs collection, without rerunning or modifying raw samples.

P1 remote Controller benefit remains unconfirmed.

## P3 comparison: CPU and capture-copy cost reduced

The operator confirmed normal display after replacing the local Host with pushed
Debug `b157e88`. All nine matched windows completed; all 7,851 traced frames were
sent, with zero capture/encode/transmit failure increments and no Host/GUI trace
overflow. All nine original/copied CSV pairs were byte-identical, all GUI traces
parsed successfully, and the hidden mirror had zero refresh increments.

Three-run medians [minimum, maximum], compared with P2 on this same Host:

| Metric | P2 dynamic | P3 dynamic | P2 dynamic + logs | P3 dynamic + logs |
| --- | --- | --- | --- | --- |
| Sent FPS | 21.32 [21.30, 21.33] | 21.28 [21.28, 21.33] | 21.28 [21.25, 21.28] | 21.28 [21.27, 21.28] |
| Host CPU, one core % | 61.90 [61.44, 62.87] | 55.34 [54.79, 55.82] | 61.41 [60.10, 61.56] | 54.88 [54.84, 55.55] |
| Capture copy, ms/successful capture | 9.78 [9.64, 9.80] | 7.25 [7.23, 7.28] | 9.72 [9.60, 9.72] | 7.25 [7.25, 7.27] |
| Encode P95, ms | 29.90 [27.06, 30.83] | 27.07 [26.87, 29.35] | 27.11 [25.44, 27.43] | 27.47 [26.72, 29.27] |
| Frame age P95, ms | 49.24 [48.96, 49.63] | 49.57 [49.31, 50.03] | 48.24 [47.48, 50.58] | 49.23 [48.35, 50.10] |
| Frame age P99, ms | 63.85 [60.51, 65.27] | 65.08 [63.87, 66.50] | 61.47 [58.35, 67.51] | 63.47 [61.53, 63.71] |

Host CPU medians fell by 10.59% / 10.63%; capture-copy time fell by 25.79% /
25.38%. The before/after ranges do not overlap for either metric. Copy summaries
are weighted by successful captures and remain supporting 10-second diagnostics,
not per-frame timing. Every retained P3 geometry summary reports three large CPU
allocations, 24,883,200 retained pool bytes, zero pool exhaustion and zero native
image copies. Readbacks still track real CPU captures: this is buffer reuse and
copy removal, not GPU-only encoding or zero-copy capture.

Latency improvement is unconfirmed: frame-age tails overlap P2 and their medians
did not fall. P3 pacing stayed at 11,076 kbps; P2 used 12,434 kbps for dynamic and
10,389–12,434 kbps for logs. Ordinary-frame mean token waits were 10.16 / 10.17 ms
in P3 versus 9.04 / 10.35 ms in P2; send-call means were 0.85 / 0.86 ms. The
capture-return-to-main-publication P99 remained around 11.55–11.58 ms, supporting
the next P4 investigation of synchronous thumbnail work. Do not add capture-copy
and frame-age percentiles or infer remote end-to-end latency from these values.

Static windows sent 187 / 0 / 0 frames and remain unsuitable for a performance
claim because first-window activity differs between runs. The dynamic fixture
still limits the measured rate to about 21.3 FPS. Evidence is retained under
`build/reports/x00-t27/p3-comparison-20260929/`, including `analysis.json`,
`trace-integrity.json`, `gui-validation.json`, geometry/counter snapshots and raw
traces. P4–P6 and the final 5-minute quality run remain.

## P4 publication-stage audit from retained CPU recordings

The later P5 CPU reference was built from unmodified P4 `af637da`, not the P5 GPU
implementation. `b157e88..af637da` contains only the P4 implementation commit.
It can therefore supply a same-machine CPU-path observation against P3 without
restarting either currently connected endpoint. All twelve dynamic traces were
reparsed and checked for timing order, geometry, successful outcomes and overflow:
7,664 P3 frames and 7,673 P4 frames, with no failed or missing rows.

| Capture return to main-frame publication, three-run median [range] | P3 | P4 |
| --- | --- | --- |
| Dynamic mean, ms/frame | 0.5410 [0.5372, 0.5440] | 0.00389 [0.00384, 0.00392] |
| Dynamic P99, ms | 11.545 [11.101, 11.579] | 0.012 [0.012, 0.012] |
| DynamicLog mean, ms/frame | 0.5534 [0.5288, 0.5706] | 0.00384 [0.00378, 0.00388] |
| DynamicLog P99, ms | 11.575 [11.171, 11.602] | 0.012 [0.011, 0.012] |

Publication delays above 1 ms fall from 175 to zero in each three-run scenario.
This supports P4's specific removal of synchronous thumbnail work before main
publication. It does not mean thumbnail work itself became free: bounded input
preparation follows publication and JPEG/send work runs in the worker.

Capture, viewport and encoded dimensions, DDA/CPU input, scene seed/cadence,
warmup, measurement duration and log visibility match. **Network configuration
identities differ across the intervening role changes**, and adaptive pacing also
differs. This is a scoped publication-stage observation, not the originally
specified fully matched network comparison. Total Host frame-age P99 is actually
higher in this P4 recording (76.043/73.180 versus 65.075/63.467 ms for
Dynamic/DynamicLog), so no total latency improvement is claimed. The existing
five-minute P5 continuity/visual result remains separately scoped.

Recompute with `python build/reports/x00-t27/analyze_p4_publication.py`;
`p4-publication-audit.json` retains per-window denominators, hashes, matched
conditions and limits. Original recordings remain under
`p3-comparison-20260929/` and `p5-cpu-reference-20260929/` in that report directory.

## Role reversal on 2026-09-29: local Client observation

The operator confirmed normal replaced video with this machine acting as Client
and the peer as Host. Current local artifact identity is P4 `af637da`. The old
local-Host status is stale; its B0/P2/P3 measurements remain specific to that
machine and role and are not a comparison baseline for the remote Host.

A natural-scene 60-second GUI trace recorded 1,749 displayed frames (29.15 FPS),
zero trace overflow and dispatch-wait P95/P99 7.990/19.644 ms. Present-call P95/P99
was 0.305/0.375 ms. Surrounding playback snapshots reported advancing D3D11 decoded
surfaces with no additional decode/present failures, CPU transfers, software
frames, surface fallbacks or resizes. The decoded visible image stayed 1584×990
and the Client output viewport stayed 1920×991. The runtime's own decoded/rendered
counters are zero on this GUI presentation route; GUI playback/trace counters
provide the actual evidence, not those runtime-only counters.

Remote Agent preflight was concurrent. This is a connectivity and observation
check, not a fixed-scene comparison, maximum-throughput result or P1 improvement
claim. Host-side build/feature identity and capture geometry must come from the
remote endpoint's sanitized result before a new fixed-scene contract is set.
No source, network, viewport or runtime replacement occurred in this observation.
Evidence: `build/reports/x00-t27/client-natural-preflight-20260929/`.

The existing remote Agent task completed after one provider-error recovery. Its
sanitized result reports capture 1680×1050 / encode 1584×990 and P2 present;
running Host commit and P3/P4 presence remain unknown. Binary hash, network
configuration digest and diagnostic interval were not established, so fixed-scene
baseline readiness is unconfirmed. No scene or load was started. The GUI result
API retained only the final 100 streamed chunks; a concise restatement recovered
these result fields without repeating diagnostics. The endpoint retained raw
evidence under opaque reference `97283b6b13bd43b0b04382d6c757fe29`; the received
short result is `build/reports/x00-t27/remote-role-preflight-short-result.txt`.

## Sending wait attribution

The subsequent [remote-Host baseline](video-link-remote-host-baseline-20260929.md)
establishes the new role's identity, nine valid windows and current CPU/readback
cost. It supersedes the pending preflight state above; the historical local-Host
results in this document retain their original scope.

The [P6 attribution report](video-send-attribution-20260929.md) reuses all 27 B0/P2/P3
windows and separates key/ordinary frames, exclusive waits, wake overshoot, send
calls, queue time and frame age. It preserves this former-local-Host scope and
records the independently scoped X00-T28 follow-up. No new live load was used.
