# X00-T27: baseline after switching the peer to Host

The current remote Host sustains about 30 sent traced FPS in the dynamic scene.
Its main remaining preparation costs are CPU capture copying and encoder input
preparation. This is a baseline for P5, not a before/after performance claim.
The [task ledger](../runtime/MODULE_KANBAN.md) tracks implementation and acceptance
separately; [earlier measurements](video-link-baseline-20260928.md) used the other
machine as Host and cannot serve as this machine's control group.

## Identity and fixed conditions

- Local Client: published Debug `af637da`, executable SHA256 prefix `0F69D712419A`.
  Remote Host: manifest-verified running SHA256 prefix `332CB8193DC4`; source commit
  unknown, P3/P4 present. Same-version equality was not required.
- WGC capture: 1680×1050 physical pixels; encoded image: 1584×990; Client viewport:
  1920×991. Geometry was retained, with no resolution change for this measurement.
- All 46 in-window codec diagnostic records agree: `h264_qsv`, nominal 30 FPS,
  time base 1/30, GOP 60 frames, configured 3763 kbps, input
  `scale-nv12-convert`, GPU scaling false. This is the active codec configuration,
  not the first line of the retained rotating log, which can describe old sessions.
- Network configuration digest prefix `B5FEEEDD17AD`; diagnostics every 10000 ms;
  main log visible, mirror hidden. Full identities and configuration evidence are
  retained in generated results and on the endpoint, not in source configuration.
- Static, Dynamic and DynamicLog each have three 10-second warmups and 60-second
  traces; seed 2700, fullscreen/topmost, scene target 30 FPS. Replay uses the
  existing fixed four-lines-per-50-ms fixture. Measured status intervals retain
  their own durations; replay line counts are not divided by trace duration.

The existing Agent collected nine windows. With the operator's additional
terminal authorization, terminal queries retrieved results and completed two
replacement windows after an explicit Agent handoff/interruption. One progress
query accidentally serialized PowerShell string metadata into a 2,950,898-byte
reply. Original Static-3 and Dynamic-1 were conservatively excluded, retained,
and replaced using the same binary, seed, geometry and configuration. Subsequent
queries used bounded primitive projections. No competing scene collector ran.

Adaptive pacing was 18305 kbps in the seven retained original windows and 16870
kbps in the two replacements. Codec bitrate stayed 3763 kbps. Thus even these
three-run ranges include adaptation and time variation; pacing-sensitive values
are not controlled-rate causal comparisons. No bitrate, burst, congestion,
priority or queue policy was changed.

## Measurements

Values are median [minimum, maximum] across three windows. CPU 100% means one
logical core, not the whole machine. Capture-copy and input-preparation means
weight 10-second summaries by successful captures and encode attempts,
respectively. Encode duration includes input preparation; do not add it again.

| Metric | Static | Dynamic | Dynamic + logs |
| --- | --- | --- | --- |
| Sent trace FPS | 29.18 [29.13, 29.35] | 30.00 [29.78, 30.00] | 29.58 [29.50, 29.80] |
| Host runtime CPU, % of one core | 63.51 [61.13, 64.61] | 91.45 [89.18, 95.82] | 102.14 [99.98, 105.58] |
| Host GUI CPU, % of one core | 37.34 [35.82, 39.64] | 47.00 [46.69, 53.93] | 115.25 [114.74, 116.71] |
| Capture copy mean, ms/capture | 4.13 [4.03, 4.33] | 4.36 [4.21, 4.38] | 5.20 [5.16, 5.23] |
| Encoder input preparation mean, ms/attempt | 10.05 [9.84, 10.11] | 12.90 [12.52, 13.97] | 15.33 [14.70, 15.34] |
| Encode P95, ms | 24.23 [23.96, 25.71] | 30.43 [30.15, 33.83] | 36.01 [35.87, 37.00] |
| Capture return → publication P99, ms | 0.018 [0.014, 0.019] | 0.019 [0.018, 0.021] | 0.023 [0.022, 0.024] |
| Publication → last send P95, ms | 59.09 [57.28, 60.11] | 56.59 [55.99, 61.28] | 63.70 [63.12, 64.26] |
| Publication → last send P99, ms | 113.95 [112.98, 116.88] | 64.22 [64.14, 67.26] | 74.73 [68.60, 77.46] |

Nine traces contain **15980 sent frames**, each covering exactly 60 seconds, with
zero unsent outcomes or Host/GUI trace overflows. Sampled status intervals record
zero synthetic frames and zero capture, encode or transmission failure increments.
All traces retain one capture generation and the fixed encoded dimensions.

The dynamic scene actually paints 19.95–21.28 FPS, while the video submits about
30 FPS; unique content FPS was not measured. The static scene paints once but
WGC media continues around 29 FPS. These values are not maximum throughput tests
and do not establish 30 distinct content frames per second or filler generation.

Every geometry snapshot retains three CPU buffers, **21168000 bytes**, zero pool
exhaustion and zero native-image copies. CPU-copy and GPU-readback counters are
equal and keep increasing. Encoder-level `encoder_gpu_to_cpu_readback=false`
only describes work inside the encoder: capture has already read the texture
back to CPU memory. This is still a CPU input path despite the QSV backend name.

Thumbnail snapshots report submitted = sent, zero failures/replacements/discards
and no pending bytes. These are cumulative snapshots, not per-window rate
denominators. The short publication timing is current-path evidence; there is no
same-peer pre-P4 recording to establish P4's improvement magnitude. Host working
set spans 149.67–156.00 MiB for runtime and 157.37–178.23 MiB for GUI across the
selected windows. This range alone does not prove absence of a long-term leak.

For ordinary dynamic frames, token waiting averages 5.66 [4.98, 6.33] ms/frame,
send calls 2.16 [2.11, 2.25] ms/frame and wake overshoot 4.17 [3.73, 4.54] ms/frame.
Overshoot is included in waiting and cannot be added as another stage. These
observations support the separately scoped X00-T28 investigation, but the full
[P6 attribution](video-send-attribution-20260929.md) still describes the former
local Host. No network or thread policy was tuned during this baseline.

## Validation and limits

A parallel low-frequency Client continuity observer took 173 samples over
1804.34 seconds: decoded D3D11 surfaces advanced by **51575**; decode failures,
present failures, CPU transfers, software frames, surface fallbacks and resizes
did not increase. This covers baseline and intervening natural desktop activity,
not a scene-aligned Client P1 comparison or the final five-minute quality test.
The closing status confirms both channels connected/streaming, the fixed Client
viewport and inactive Host log replay. Owned scene collectors have completed;
the current Host was not restarted or replaced.

Generated evidence under `build/reports/x00-t27/` includes:

- `remote-role-baseline-contract-20260929.json`: fixed role/geometry/configuration.
- `remote-role-baseline-summary.json`: nine sanitized windows, source CSV hashes,
  scene/counter denominators, active codec configuration and actual pacing.
- `remote-role-effective-codec.json`, `remote-role-pacing-summary.json`: bounded
  in-window configuration checks, excluding stale log lines.
- `remote-role-baseline-validation.json`, `validate-remote-baseline.py`: reproducible
  result validation and three-run medians/ranges; 9 windows / 15980 frames pass.
- `remote-role-baseline-query-interference.json`: excluded originals and completed
  replacements; raw traces and endpoint-local analysis remain on the peer.
- `client-role-baseline-observer-20260929/` and both post-baseline status receipts:
  continuity and cleanup checks.

This phase changes analysis/documentation, not the application. The previously
verified `af637da` Debug build and running publication remain unchanged; no main
rebuild was needed. P6's four offline regressions and 27 historical trace-window
checks passed separately. There is no same-peer before/after comparison, new
P1 benefit claim, calibrated end-to-end latency, final five-minute visual-quality
acceptance or P5 GPU-input implementation in this result.

The subsequent [P5 candidate and local validation](video-gpu-input-p5-20260929.md)
implement shared D3D11 synchronization, format preparation, first-output GPU
activation and bounded CPU fallback. This baseline predates that candidate.
Capture readback counts and input-preparation cost remain the comparison targets;
nominal hardware acceleration alone is insufficient.
