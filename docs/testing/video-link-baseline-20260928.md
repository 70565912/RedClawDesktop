# X00-T27 local Host baseline, 2026-09-28

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

Dynamic capture-copy weighted means are 9.62–9.69 ms per capture attempt, and
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
`build/reports/x00-t27/analyze_baseline.py` recomputes nearest-rank percentiles and
three-run summaries. Raw reports remain local because they contain endpoint data.

P1 implementation/local replay is verified; remote Controller benefit remains
unconfirmed. P2 implementation/local tests and its next pushed Debug Host are the
next checkpoint. P3–P6 and the final 5-minute dynamic/decoded-quality run remain.
