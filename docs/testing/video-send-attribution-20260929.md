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
