# X00-T27 P6: sending wait attribution

The recorded local Host is predominantly waiting for pacing tokens. The evidence
also shows substantial wake lateness inside those waits. It does not establish
that changing bitrate or removing the same number of milliseconds would improve
end-to-end latency. This report covers the **former local Host**, not the current
remote Host. Current topology and pending validation are on the
[task ledger](../runtime/MODULE_KANBAN.md).

## Inputs and accounting

The existing B0, P2 and P3 recordings contain 27 windows and 24,822 sent traced
frames, with zero trace overflow or unsent outcomes. Each phase has three static,
three dynamic and three dynamic-with-logs windows. Static activity differs and is
excluded from performance comparisons. Dynamic scene throughput is about 21.3 FPS,
limited by the scene generator. Capture is 1920×1080; encoding is 1778×1000.

| Recording | Host commit | Dynamic pacing, kbps | Log-scene pacing, kbps |
| --- | --- | --- | --- |
| `b0-aligned-20260928` | `808c144` | 16030 | 16030 |
| `p2-comparison-20260928` | `c9df422` | 12434 | 12434 / 10389 / 10389 |
| `p3-comparison-20260929` | `b157e88` | 11076 | 11076 |

All three recordings already used nominal 30 FPS during these windows. They do
not exercise the old nominal-1-FPS initialization defect. Network configuration
was held, but adaptive pacing rates were not equal across phases.

The analyzer emits schema `redclaw.video-link-baseline-analysis.v3`, retaining
trace SHA256, generation, configuration and outcome denominators. Each class's
latency uses only successfully sent frames. It verifies admission bounds,
duplicate frame identity, ordered timestamps, fragment completion, exclusive
wait sums and nonnegative pacer remainder. Empty classes retain unavailable
quantiles instead of reporting zero latency.

Pacer duration runs from pacer dequeue to completion. Its exclusive components
are token, in-flight, buffered and channel waits, send calls, transport-state
queries, callbacks and the uninstrumented remainder. Queue time is measured
separately from enqueue to pacer dequeue. Frame age runs from capture publication
to the final send call; it excludes earlier capture/copy and remote presentation.
Quantiles are nearest-rank within a window, followed by three-run median and range.
Shares divide summed component duration by summed pacer duration in that window.

Wake overshoot is the sum of positive `elapsed - requested` wait durations. It is
already included in a wait reason and must not be added again. These data cannot
separate scheduler delay, timer resolution and mutex reacquisition, or count
individual timeout/wakeup events. Wait reasons follow the runtime's exclusive
channel/in-flight/buffered/token priority; they are not independent causal effects.
`probe_wait_us` is unpopulated in v1 and is not evidence of zero probe effects.

## Before and after P2

Dynamic-scene values are medians [minimum, maximum] over three windows. Frame
sizes are wire bytes, including fragment headers, rather than pure codec payload.

| Metric | B0 | P2 | P3 |
| --- | --- | --- | --- |
| Keyframe share, % | 1.64 [1.64, 1.72] | 1.72 [1.64, 1.80] | 1.72 [1.72, 1.80] |
| Keyframe mean bytes | 31642 [31411, 31740] | 31478 [31049, 31784] | 31448 [31240, 31577] |
| Ordinary mean bytes | 17635 [17573, 17664] | 17652 [17527, 17669] | 17660 [17572, 17694] |
| Keyframe mean pacer, ms | 16.86 [15.55, 18.53] | 20.54 [20.39, 21.04] | 21.11 [20.19, 21.65] |
| Ordinary mean pacer, ms | 8.55 [8.36, 8.68] | 10.12 [9.97, 10.22] | 11.16 [11.08, 11.20] |

Keyframe sizes and ratios overlap. No increase in keyframe size explains the
higher pacing duration; lower adaptive pacing and wake lateness are concurrent
factors. This is not an isolated causal P2 regression or improvement. Each window
has only 21–23 keyframes, so their P99 is effectively a maximum and is unstable.

## P3 attribution and next investigation

Values below are three-window medians. Detailed ranges and every class/metric are
in the generated JSON. All instrumented in-flight, buffered and channel waits
are zero in these windows; they were not the recorded blocking predicates.

| Scene / frame class | Token wait, ms/frame | Send calls, ms/frame | Token share of pacer | Overshoot, ms/frame |
| --- | --- | --- | --- | --- |
| Dynamic / key | 19.67 | 1.33 | 92.72% | 9.97 |
| Dynamic / ordinary | 10.18 | 0.85 | 91.17% | 5.92 |
| Dynamic + logs / key | 20.15 | 1.52 | 91.84% | 11.07 |
| Dynamic + logs / ordinary | 10.17 | 0.87 | 90.96% | 6.00 |

For ordinary dynamic frames, overshoot averages 5.92 [5.81, 6.03] ms/frame and is
58.10% [57.52%, 59.00%] of recorded wait time. Its per-frame P99 is 15.83
[15.74, 16.38] ms. Queue-wait P99 is 20.93 [20.17, 21.03] ms; final-send frame-age
P99 is 63.12 [61.50, 66.20] ms. These are different distributions and cannot be
added. Uninstrumented pacer work is below 1% for these dynamic class windows.

The follow-up `X00-T28` should first reproduce wake lateness under a matched
current Host baseline, then evaluate a bounded, cancellation-aware wake mechanism
against the existing wait. Hold bitrate adaptation, burst allowance, congestion
control, priority and queue capacity fixed. Measure wait overshoot, CPU, frame-age
tails and stop/reconnect correctness; retain the current behavior if improvement
falls within run variation. This task makes no runtime scheduling change.

## Reproduction and validation

From the repository root, recompute each recording independently:

```powershell
python scripts/capture/analyze-video-link-baseline.py build/reports/x00-t27/b0-aligned-20260928 --output build/reports/x00-t27/p6-b0-attribution.json
python scripts/capture/analyze-video-link-baseline.py build/reports/x00-t27/p2-comparison-20260928 --output build/reports/x00-t27/p6-p2-attribution.json
python scripts/capture/analyze-video-link-baseline.py build/reports/x00-t27/p3-comparison-20260929 --output build/reports/x00-t27/p6-p3-attribution.json
python scripts/capture/test-analyze-video-link-baseline.py
```

All 27 actual windows passed the accounting checks; previously computed window
metrics and three-run summaries remain exactly equal to the retained reports.
Four offline regression cases
passed, covering hand-calculated exclusive accounting, failed/empty classes,
invalid timing/fragment/wait rejection and three-run variation. No application
source changed; no main-program rebuild or live test load was needed. Raw data
remain local. The current-role P4 comparison, P5 GPU input and final quality run
remain separate work; no cross-machine clocks were subtracted.

## In-flight stall investigation (X00-T30, 2026-09-30)

The retained P5 DynamicLog-2 trace identifies frame 14176 as an in-flight
admission drop: 16063 wire bytes, zero sent fragments, 1023.429 ms of in-flight
wait and 1027.583 ms from pacer entry to completion. Neighboring sent frames
14175 and 14178 are separated by 1101.005 ms; 14178 is an IDR and completes
46.912 ms after the drop. Initial RTT is 9 ms; the next frame records 125 ms.
The failed frame has no token, buffered or channel wait and no send call. These
are exclusive blocking-predicate observations, not a diagnosis of peer/network loss.

The original trace SHA256 is
`f24b035c982eabe8914df16428a1bc6e77bf0a80e7c3d3e6d5cefe0ef158eacc`.
Recomputed neighbors, the full failed row and derived guards are retained in
`build/reports/x00-t27/x30-historical-attribution.json`. The original runtime's
remaining rotating logs begin after 04:51, while this incident occurred around
00:38. The baseline copied capture/codec summaries, not transport-feedback
history. GUI samples cache diagnostic counters and cannot reconstruct each ACK.
Missing initial/final in-flight bytes, policy horizon and refresh timestamps
prevent assigning the historical event to an ACK outage or a state-update race.

### Locally established mechanism

`MediaTransportEstimator::snapshot()` performs expiry; the pacer holds a copy
of its in-flight byte count. The runtime refreshes that copy on feedback and
approximately one-second Host heartbeats. At RTT 9 ms, expiry is 750 ms. Without
new feedback, a packet that has just missed a heartbeat's expiry check can stay
in the pacer's budget until the following heartbeat. Wall-clock passage alone
does not expire that copied budget.

For 16063 bytes at 8966 kbps the initial pacing allowance is 15 ms and the guard
250 ms. With a nonzero initial in-flight count, the expiry extension produces
1015 ms before any larger feedback-horizon guard or subsequent rate/RTT update.
This is consistent with the observed duration but does not prove the event's
actual deadline. For example, a packet sent at 0.5 s remains live at the 1.0 s
heartbeat and is expired at 2.0 s; a frame admitted at 0.6 s with a 1015 ms
deadline can drop at 1.615 s before that refresh.

The isolated fixture uses the real estimator and pacer with an open channel:

| Event while blocked | Result | Evidence in the new trace |
| --- | --- | --- |
| Media ACK followed by budget refresh | Sends the frame | ACK sequence advances; expiry count unchanged |
| Expiry snapshot followed by budget refresh | Sends the frame | Expiry count advances; ACK sequence stays zero |
| No snapshot until after the deadline | Drops without sending | Final estimate timestamp remains old; later snapshot retires the packet |

The last case establishes a local failure mechanism, not that the remote peer
stopped acknowledging the historical frame. No congestion, deadline, rate,
queue-size or wake policy has been changed.

### Bounded trace v2

Completed CSV exports now identify `redclaw.host-frame-trace.v2`. Existing
columns retain their meaning. Added counters distinguish predicate-notified
wakes (including lifecycle cancellation) from wait timeouts. Each frame retains
the first in-flight block and final admission snapshots: Host observation,
budget-update and estimate times; latest current-revision feedback time/id;
ACK and last-recorded-send sequences; oldest retained packet send time;
expired-packet count; estimator and pacer byte counts; byte limit; accepted
budget-update count, rejected older-policy count and policy revision.
`blocked_wire_bytes` identifies the actual fragment that failed admission.

Snapshot fields are internally consistent under their owner's mutex, but the
estimator and pacer snapshots are not one atomic network observation. A difference
in their byte counts alone does not prove a race. Zero estimate provenance means
that update carried no estimator snapshot. First-block fields stay zero if no
in-flight wait occurred. Intermediate ACK/update events are not a full packet
history, and stale-revision feedback can retire bytes without updating the
current-revision timing fields. Do not infer one-way latency from peer clocks.

Capture remains opt-in, at most 4096 rows and 120 s, with the existing late-frame
completion allowance. The row storage is checked to remain below 4 MiB; no
per-frame disk writes occur while sampling is off. Existing v1 arm requests
remain accepted and the baseline analyzer accepts both export versions. No
media/control protocol changes or equal-version Client requirement are added.

### Verification and next evidence

The focused suites cover trace bounds/export and disabled behavior, ACK/expiry/
missing-refresh attribution, old-policy rejection, media packet/feedback
accounting, adaptive sending and recovery. During validation, the existing
`UnconfirmedProbeHasHardByteLimitAndCanBeCancelled` assertion also failed on the
unchanged `532f1a0` checkout with the same 114688-versus-65536-byte result. It
expected cancellation to drop all media, contrary to the established sender
behavior. The fixture now checks that probe-tagged bytes stay capped while a
sendable frame continues and completes after probe cancellation. Production
probe policy is unchanged. Pre-change evidence is `x30-before-probe.xml` under
the same local report directory.

All four affected CTest targets pass (`redclaw_host_frame_trace_tests`,
`redclaw_net_video_frame_transport_tests`, `redclaw_adaptive_media_sender_tests`,
`redclaw_transport_recovery_regression_tests`, 34.39 s total), along with the
four cases in `python scripts/capture/test-analyze-video-link-baseline.py`.
The retained DynamicLog-2 metric arithmetic is identical when replayed with a
synthetic v2 header; this checks reader compatibility, not live v2 evidence.
Main Debug NoPublish compilation passes through `build.ps1`. Receipts are
`x30-ctest.xml`, `x30-ctest.txt`, `x30-reader-check.json` and `x30-main-build.txt`
under the report directory. The first build invocations used an incompatible
PowerShell/compiler environment; explicit Windows PowerShell and the existing
VS2022 environment entry corrected that without changing build policy.
Publication/replacement follows the existing pushed-source controlled-Host procedure.

Next, after the candidate is published and the operator confirms remote Client
picture, record a bounded DynamicLog window with v2 trace and retain transport
summaries immediately. Compare blocked/final snapshot age, ACK progress, expiry
and update counters before choosing a repair. X00-T30 remains open until that
live attribution is available; no measured latency improvement is claimed.

### Pushed diagnostic candidate and Host readiness

`8301b4b40aea2e76e97539821131ebe133ffa6bb` is pushed to `origin/main` and
clean-source Debug built/published through `build.ps1`. Candidate command entry
and build/publication identity match; all 336 candidate files are verified.
The independent lifecycle worker completed the local Host replacement with a
complete 336-file rollback bundle. Formal and rollback manifests were verified
afterward. The deployed executable SHA256 is
`9cb68a7ce5392bf8eb2f780b59bd9f0be3e943575ebc11c1511efc2dca93a77d`.

Fresh readiness is `dht_waiting`, published DHT, retained ICE port 56000,
`connected=false`, zero captured/transmitted frames. These prove readiness,
not remote media acceptance. The operator has been asked to reconnect and
confirm picture/unchanged viewport; no trace or dynamic load has started.
Local receipts under `build/reports/x00-t27/` are `x30-candidate.json`,
`x30-candidate-manifest.json`, `x30-pushed-build.txt`,
`p5-acceptance-x30-dispatch.json` and `p5-acceptance-x30-deployed.json`.

### Reconnected live v2 capture

The operator confirmed the candidate picture and unchanged viewport. Preflight
verified real capture, original network settings, 1920×1080 capture, 1778×1000
encoding, 1920×1001 viewport, NVENC GPU input, 4267 kbps, nominal 30 FPS,
time base 1/30 and GOP 60. Three fixed DynamicLog runs used seed 2700, at least
10 s warmup, 60 s trace windows, visible Host log, hidden mirror and the existing
four-lines-per-50-ms replay. Only these affected windows were repeated.

| Window | Sent trace frames | In-flight blocked frames | Failed/cancelled frames | Host frame-age P99, ms |
| --- | --- | --- | --- | --- |
| DynamicLog-1 | 1281 | 0 | 0 | 47.106 |
| DynamicLog-2 | 1281 | 0 | 0 | 48.106 |
| DynamicLog-3 | 1280 | 0 | 0 | 47.136 |

All 3842 v2 rows pass identity, timing, fragment, exclusive-wait and wake-count
checks without overflow. First-block fields correctly stay zero; final snapshot
provenance is populated. Maximum final estimator-snapshot ages are 189.053,
195.398 and 164.977 ms. ACK sequences advance throughout; expiry and rejected
older-policy counts remain zero. Eighteen complete, unique codec/transport
diagnostic windows retain the fixed codec configuration and zero GPU fallbacks.
Within each window's retained transport-summary bounds, expiry, lost-packet,
ignored-feedback, deadline-drop and send-failure counters do not increase.
Those summary deltas do not cover precisely the same boundaries as the frame
trace. Sampled capture/encode/transmit failures also have zero deltas.

The incident did not recur. This validates the diagnostic candidate and the
bounded run's stability, but **does not establish the historical ACK/network
cause or a latency improvement**. Expiry-refresh lag remains a locally
reproduced mechanism, not a confirmed explanation of frame 14176. Keep X00-T30
historical attribution unresolved rather than treating absence as a repair.
No continuing background monitor or workload was installed.

The same recording supplies a current-role baseline for the independently
actionable X00-T28 wake investigation. Ordinary-frame token waits account for
a three-run median 90.75% [90.74%, 90.87%] of pacer duration. Mean overshoot is
5.947 [5.910, 5.995] ms/frame and 60.14% [59.77%, 60.21%] of recorded wait.
Pacing remains 11217 kbps in these windows, while actual output is about
21.33–21.35 FPS. Queue P99 is 21.055 [20.106, 24.799] ms. Overshoot includes OS
scheduling and mutex reacquisition; it does not isolate a timer-resolution cause.
These are a baseline, not a before/after claim against the earlier 8966 kbps run.
Proceed with X00-T28's bounded wake comparison while retaining X00-T30 trace
support for a future recurrence; do not change congestion or expiry policy as
part of that comparison.

Recompute with `python build/reports/x00-t27/analyze_x30_admission.py`.
Raw and derived records are under `build/reports/x00-t27/x30-admission-20260930/`,
including `admission-analysis.json`. The local collector is
`run-x30-windows.ps1` in the parent report directory; it retains transport
numeric summaries alongside capture/codec summaries before log rotation.
After collection, the same Host remains connected with real media, log replay
is inactive and no scene process remains. This is a diagnostic/code-validation
stage completion; historical root-cause attribution remains open.
