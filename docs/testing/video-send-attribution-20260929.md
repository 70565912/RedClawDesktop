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

## Bounded pacer wake replacement (X00-T28, 2026-09-30)

The reference is the three X00-T30 DynamicLog windows above, on local Host
`8301b4b`: ordinary-frame wake overshoot 5.947 [5.910, 5.995] ms/frame,
queue P99 21.055 [20.106, 24.799] ms and Host frame-age P99
47.136 [47.106, 48.106] ms. Preserve this baseline, including the effective
11217 kbps pacing rate, for the subsequent real-media comparison.

`MediaPacerWait` owns one Windows high-resolution one-shot timer and one
notification event per pacer. The pacer worker uses it for fragment-admission
and frame-cadence deadlines. Predicate state and timer reset/arming use the
existing pacer mutex; blocking releases that mutex. Stop, reset, writable and
budget updates notify both timed and idle/drain waiters. Worker completion
still wakes reset/drain waiters. All handles outlive the joined worker.
Unrelated notifications do not grant credit or extend the original deadline.

Timer creation/arming/wait failures latch the condition-variable fallback.
Other platforms use that fallback directly. No spin loop, global timer-resolution
change, thread-priority change or extra worker is introduced. Local periodic
diagnostics add `pacer_high_resolution_wait`; frame trace remains v2, with no
wire or persisted-protocol change. The timer API and relative one-shot behavior
follow Microsoft's [CreateWaitableTimerExW](https://learn.microsoft.com/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)
and [SetWaitableTimerEx](https://learn.microsoft.com/windows/win32/api/synchapi/nf-synchapi-setwaitabletimerex)
contracts.

Bitrate, congestion, in-flight expiry/refresh, hard deadlines, queue limits,
token accounting and burst formulas are unchanged. The existing burst formula
uses measured wake lateness, so its effective result can adapt to a different
waiter; it is not artificially frozen. Compare actual pacing and wait counts as
well as overshoot, CPU and frame age. A primitive-only timing gain cannot prove
real-media or end-to-end latency improvement.

Local validation passes:

- `build.ps1 -Configuration Debug -FreshConfigure -Target redclaw_desktop -NoPublish`.
- Five focused CTest suites: `redclaw_media_pacer_wait_tests`,
  `redclaw_adaptive_media_sender_tests`, `redclaw_transport_recovery_regression_tests`,
  `redclaw_net_video_frame_transport_tests` and `redclaw_host_frame_trace_tests`.
  The eight new cases cover stale notifications, predicate changes at unlock,
  wake storms with a fixed deadline, no early cadence release, reset/stop while
  admission or cadence is waiting, and explicit condition-variable fallback.
- Three isolated rounds per backend, alternating order. Each warms up 32 waits,
  then measures 512 waits cycling through 100/250/500/1000/2000/4000/8000/16000 us.
  All 1536 automatic waits use the native backend; neither backend returns
  before its requested interval. CSV writing occurs after measurement.

| Primitive metric, three-run median [range] | Condition variable | Native timer |
| --- | --- | --- |
| Mean overshoot, ms/wait | 13.210 [13.175, 13.248] | 0.388 [0.362, 0.417] |
| Overshoot P95, ms/wait | 15.528 [15.393, 15.537] | 0.611 [0.581, 0.634] |
| Overshoot P99, ms/wait | 15.657 [15.618, 15.826] | 0.853 [0.738, 1.013] |
| Process CPU for 512 waits, ms | 15.625 [0, 31.250] | 15.625 [0, 31.250] |

The isolated overshoot reduction is established. CPU counters have an observed
15.625 ms granularity, so these short runs do not resolve a CPU benefit or small
regression. The fixed primitive sequence is **not** the live video's wait
distribution: do not compare its per-wait numbers directly with per-frame
DynamicLog overshoot or claim end-to-end gains. Runtime native API failure was
not fault-injected; the fallback's deadline/cancellation behavior is exercised
explicitly. Other operating systems have not been run in this Windows check.

Reproduce with developer-only `redclaw_media_pacer_wait_probe automatic` and
`redclaw_media_pacer_wait_probe condition-variable`; the probe is excluded from
default builds and is not a CTest performance gate. Raw CSVs, SHA256 identities
and `analysis.json` are under `build/reports/x00-t27/x28-wake-probe-20260930/`;
recompute using `python build/reports/x00-t27/analyze_x28_probe.py`. Build/test
logs are `x28-main-build.txt`, `x28-focused-build.txt`, `x28-ctest.txt/xml` in the
parent directory.

The serving Host is still the X00-T30 reference at this implementation checkpoint.
Candidate publication and controlled replacement follow push; matched DynamicLog
sampling awaits the operator's new-candidate picture confirmation. X00-T30's
historical in-flight cause remains unresolved and its diagnostic fields remain.

### Pushed candidate and controlled Host readiness

Pushed implementation `e787ca5a485135609edc23e9410fefdade24bfa7` to the existing
`origin/main`, then built and published that clean checkout with
`build.ps1 -Configuration Debug -Target redclaw_desktop`. The first push met a
transient TLS handshake failure; the unchanged retry succeeded. Candidate
entry check and all 336 manifest entries pass; the published executable matches
the build, SHA256
`4b52b24b5260b352cfb7f9358889e3a6417f2aa622c412eb35f03b991395d3f4`.

The independent lifecycle worker completed replacement into `release/Debug`.
Formal and rollback manifests each verify 336 files. Existing credentials and
ICE port 56000 are retained; the new Host's DHT listener, reachability and
publication pass. At the readiness receipt it is `dht_waiting`, not connected,
with zero new capture/transmit frames. No post-replacement performance workload
or sampling has started. Operator reconnection/picture and fixed-viewport
confirmation are pending before the three matched DynamicLog windows.

Receipts are `x28-candidate.json`, `x28-candidate-manifest.json`,
`x28-pushed-build.txt`, `p5-acceptance-x28-deployed.json` and the independent
operation directory `p5-acceptance-x28-deployment/`, all below
`build/reports/x00-t27/`. Implementation, local validation and controlled
publication are complete; real-media improvement is not yet established.

### Reconnected three-window wake acceptance

The operator reported automatic remote Client reconnection. Retained the prior
fixed-window instruction; preflight verified real captured media, unchanged
network configuration/log visibility, 1920×1080 DDA capture and 1778×1000 NVENC
encoding at 4267 kbps, nominal 30 FPS, time base 1/30 and GOP 60. All eighteen
complete codec/transport diagnostic windows confirm GPU input with zero fallback
and `pacer_high_resolution_wait=1`. No peer-version equality gate was added.

Three DynamicLog windows each use seed 2700, the same motion trajectory,
at least 10 seconds warmup and 60 seconds Host trace. The visible log replay is
about 80 lines/s and the hidden mirror remains hidden. Candidate windows send
1295/1280/1280 frames: **3855 sent, zero dropped/cancelled, zero in-flight blocks**.
All v2 rows pass identity, timing, fragment, exclusive-wait, wake-count and
overflow checks. Capture/encode/transmit failure deltas are zero; retained
transport-summary deltas show no loss, expiry, ignored feedback or send failures.
Transport summary boundaries differ slightly from the trace boundaries.

| Metric, three-run median [range] | Reference 8301b4b | Candidate e787ca5 |
| --- | --- | --- |
| Ordinary-frame overshoot, ms/frame | 5.947 [5.910, 5.995] | 0.319 [0.308, 0.326] |
| Ordinary-frame token wait, ms/frame | 9.888 [9.887, 9.958] | 3.901 [3.781, 3.959] |
| Ordinary-frame pacer duration, ms/frame | 10.896 [10.896, 10.958] | 4.750 [4.661, 4.822] |
| Send-queue P99, ms | 21.055 [20.106, 24.799] | 14.586 [11.806, 16.303] |
| Host capture-ready to last-send P95, ms | 42.834 [38.381, 42.854] | 29.649 [24.706, 31.655] |
| Host capture-ready to last-send P99, ms | 47.136 [47.106, 48.106] | 41.780 [34.551, 41.857] |
| Sent FPS | 21.350 [21.333, 21.350] | 21.333 [21.333, 21.583] |
| Runtime CPU, percent of one logical core | 11.414 [11.104, 12.191] | 11.430 [11.327, 11.953] |
| GUI CPU, percent of one logical core | 30.055 [29.319, 30.146] | 27.722 [27.657, 32.839] |
| Effective pacing, kbps | 11217 throughout | 21677 throughout |

The recorded wake overshoot is 94.6% lower, and its share of ordinary-frame
wait falls from 60.14% to 8.17%. Keyframe overshoot also falls from
11.065 [10.666, 11.486] to 0.491 [0.481, 0.510] ms/frame. The implementation
does not spin. Recorded wait counts increase from 1013/1002/997 to
1430/1345/1369 per window, while runtime CPU ranges overlap. **CPU and FPS
improvement are not established.** The scene itself measures about 21.33 FPS
in both groups; this is not a maximum-throughput test.

Queue P99 and Host frame-age P95/P99 are below the reference ranges, but this
is **not a fixed-effective-pacing experiment**. Congestion/bitrate/burst formulas
were unchanged; the adaptive controller selected 21677 instead of 11217 kbps,
and RTT samples span 4–8 instead of 4–10 ms. Thus the 30.7% queue-P99 and 11.4%
Host-frame-age-P99 reductions cannot be attributed entirely to the timer.
The isolated fixed-wait probe above supplies direct wait-mechanism evidence;
the live windows confirm the smaller recorded overshoot and overall observed
sender tails under the actual adaptive policy. No end-to-end latency is inferred.

Encoding mean is unchanged within variation (12.878 → 12.786 ms). Ordinary
wire size is essentially unchanged (17649 → 17660 bytes/frame); keyframe size
is 31726 → 31440 bytes/frame, with overlapping ranges. Runtime private memory
stays within 491.590–491.715 MiB across the candidate samples, versus
487.047–487.836 MiB in the reference; this small cross-process increase is
recorded, not described as a memory reduction. The per-window private-memory
maximum stays at 491.715 MiB in all three trials. The operator's reconnection report does not constitute
a fresh pixel comparison or separate requalification of text/line/cursor quality.

Recompute both groups with `python build/reports/x00-t27/analyze_x28_live.py`.
Candidate raw data, frame hashes and `comparison.json` are in
`build/reports/x00-t27/x28-wake-live-20260930/`; the reference is
`x30-admission-20260930/`. The inherited plan field `comparison_baseline` contains
an older label; comparison source directories and commit identities in the
analysis explicitly select these two recordings. No historical label is used
to select data. `postflight.json` confirms the same new Host remains connected,
log replay is inactive and no scene process remains.

**X00-T28 implementation, local validation, deployment and bounded live wake
acceptance are complete.** Reused the passed Debug build and five focused suites;
this sampling turn changed no production code and did not rebuild or restart.
X00-T30's historical in-flight cause remains unresolved; no new incident occurred
in these windows and no background load or monitor is left running.
