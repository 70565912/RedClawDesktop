# X00-T27 P5: synchronized GPU input

P5 is implemented and functionally validated, including the peer's Intel QSV
surface repair. **The latest matched local NVIDIA Host comparison fails the
FPS/latency objective:** GPU input removes main-video readback and reduces Host
CPU, but throughput falls and frame-age tails increase. The five-minute dynamic
run completed without stream failures or reconnects. See the
[matched acceptance below](#matched-local-host-performance-acceptance).
Earlier implementation, publication and role-specific receipts follow in order;
their waiting states do not describe the current connected Host.
The subsequent [input-cost diagnosis](#dda-shared-device-wait-diagnosis) reproduces
the regression in local probes and identifies DDA waiting under the shared
device's internal multithread protection. Runtime scheduling is not yet repaired.

## Ownership and behavior

`capture_d3d11` owns the shared device, immediate context and recursive mutex.
Capture copying/cursor work, the video processor and FFmpeg's D3D11 lock/unlock
callbacks use that mutex. D3D11 multithread protection is enabled. Native textures
and FFmpeg's device free callback retain the owner until their references are
released. Frame acquisition, networking and thread joins do not hold the mutex.
The extracted video processor reuses its pipeline and output handle.

GPU conversion accepts explicit output formats. QSV uses NV12; other backends
select advertised BGRA/NV12 support. Video-processor capabilities are checked;
visible-region copying tolerates hardware surface padding; NV12 requires even
dimensions. Cropping, aspect-preserving letterboxing and full-range BGRA to
limited BT.601 match the existing CPU route. A texture readback test found that
RGB-background handling produced Y=0 in NV12 bars; explicit Y/Cb/Cr background
values correct that to limited-range black.

The encoder owns a generation-scoped policy. Initial delivery supplies CPU and
GPU while a native path is being proven; only a real encoded packet confirms
GPU-only delivery. Failure records a reason, requests IDR and resumes from the
next real CPU frame. It does not recursively resubmit an old frame or retry GPU
each frame. Resize preserves a failed generation's CPU latch; a new generation
permits another attempt. Recovery's first CPU frame retires the old GPU context
without marking the new generation failed. Runtime orchestration consumes one
typed delivery decision instead of repeating three eligibility expressions.

Navigation preparation scales a GPU-only frame to a maximum edge of 320 before
reading back the small image. It no longer requests a full desktop CPU capture.
The bounded JPEG/send worker and generation invalidation remain. WGC's native
pool has three slots for capture, latest pending and encoding ownership.

No media/control wire format changed. Local diagnostics add GPU confirmation and
attempt/fallback counts (`encoder_gpu_input_stats_version=1`), plus thumbnail GPU
readback count/bytes (`thumbnail_stats_version=2`). Input-preparation timing now
includes GPU conversion; capture-copy timing includes waiting for the context.
The bounded trace schema is unchanged.

## Local validation

The main program passed `build.ps1 -Configuration Debug -SkipConfigure -Target
redclaw_desktop -NoPublish` using the preset's VS2022 toolchain. The published
Client remains `af637da`; no Host replacement, role switch or remote load occurred.

| Check | Result |
| --- | --- |
| Five related automatic suites: GPU policy/ownership, frame pool, thumbnail worker, capture recovery/cursor and encoder execution | Passed; the initially failing encoder suite passed after its stale object was rebuilt |
| NV12 conversion, crop/letterbox color and bounded GPU thumbnail | Passed, with texture pixel checks |
| Real encode/decode, GPU-only input, concurrent context copies/thumbnail preparation, forced failure, CPU fallback, resize and new device | Passed; failure latched once and a new generation reactivated GPU |
| WGC capture → NVENC, three device generations | Passed; 18 GPU-only frames with zero main-video readback increment |
| DDA capture → NVENC, three device generations | Passed; 18 GPU-only frames with zero main-video readback increment |
| Thumbnail readback in each real-capture probe | 18 × 230400 = 4147200 bytes, at 320×180 BGRA per image |
| Existing GPU cursor versus CPU shapes/clipping/rotation | Passed |

The real-capture probes encode 640×360 for a bounded functional gate. They are
not performance comparisons or permission to lower production resolution. The
[current-role baseline](video-link-remote-host-baseline-20260929.md) retains
production geometry and measurement conditions for subsequent comparisons.

Initial local build failures mixed MSVC 14.44 with MSVC 14.51 STL headers; loading
the matching VS2022 environment in the build process resolved that mismatch.
One old encoder-test object had also missed the changed diagnostics layout; a
debugger located the destructor crash at that ABI boundary. Removing that
generated object, fresh configuration and a focused rebuild fixed it. Compiler
checks, system-wide environment and running configuration were not changed.
The final focused relink also encountered the documented Ninja stall in the
restricted environment. The owned build was stopped, its stale lock removed
after checking no builders remained, and the focused build retried with bounded
watchdog timeouts outside that restriction. Its original log is retained.

Evidence under `build/reports/x00-t27/`: `p5-main-build-vs2022.txt`,
`p5-focused-tests.txt`, `p5-encoder-tests-recheck.txt`, `p5-encoder-final.txt`,
`p5-gpu-final.json`, `p5-cursor-gpu.json`, `p5-codec-reason-final.json` and
`p5-validation.json`. Earlier failures
and debugger receipts remain. Hardware/desktop probes are developer-invoked,
not mandatory unattended CI cases. Focused commands after building their targets:

```powershell
ctest --test-dir build/ninja-x64 -C Debug --output-on-failure -R "^(redclaw_capture_gpu_input_tests|redclaw_navigation_thumbnail_tests|redclaw_captured_frame_pool_tests|redclaw_capture_encoder_execution_session_tests|redclaw_capture_recovery_cursor_tests)$"
build/ninja-x64/tests/Debug/redclaw_capture_gpu_input_tests.exe --gtest_filter=GpuInputHardware.*:RealDesktop/GpuInputRealCapture.*
build/ninja-x64/tests/Debug/redclaw_capture_recovery_cursor_tests.exe --gtest_filter=CaptureCursor.D3D11MatchesCpuForShapesClippingAndRotation
```

GPU-only means no main-video GPU→CPU readback. GPU texture copies remain; this
is not a zero-copy claim. Remote QSV activation, AMF behavior, cross-LAN latency
and CPU improvement, and final quality/soak acceptance are unqualified. Follow
commit/push/build/controlled rollout before peer measurements, preserving the
operator's current Client/Host roles unless explicitly changed.

## Pushed candidate publication

P5 implementation `8955125` and the operator-requested GitHub policy update are
pushed to `origin/main`. Clean source `9910fe8` was then built through
`build.ps1 -Configuration Debug -Target redclaw_desktop`; configure, build and
publication passed in the existing independent checkout. Its `release/Debug`
contains 336 files. The complete manifest and required dependencies validate,
the published executable matches the build, and its `--help` entry exits with
code 0. Executable SHA256:
`f02d15357d28a74dcf081e35215de53bdcfa59ab807e0152c6ab8ee2acbb8935`.

Evidence: `p5-pushed-debug-build.txt`, `p5-candidate-manifest.json` and
`p5-publish-validation.json` under the same local report directory. Application
tests above were reused; this publication check does not repeat their matrix.
The running Client publication remains unchanged, and its controller connection
and channel are open. The candidate has not replaced either endpoint; remote
QSV activation, matched performance and visual acceptance remain unverified.

## Peer upgrade check and navigation regression

The operator subsequently upgraded the remote Host and confirmed normal video
after reconnecting the local Client. The running Host hash starts `16751dba49ba`;
its checkout is `35c8001`, but its old run manifest does not match that executable.
The manifest is retained as historical evidence, not used to assert build identity.

P5 attempts GPU input once and latches CPU fallback: `h264_qsv`, nominal 30 FPS,
1/30 time base, GOP 60, attempts 1, fallbacks 1, GPU confirmation false. The failure
is `av_hwframe_ctx_init failed for D3D11 frames: Unknown error occurred`.
Capture readbacks grow 9843→10307; three CPU allocations and zero pool exhaustion
are retained. The active backend is Desktop Duplication, whereas the baseline used
WGC. No nine-window comparison was started: GPU activation and backend matching
are prerequisites for an attributable comparison. Local NVENC success does not
qualify this Intel path. Inspect the QSV fixed NV12 texture-array allocation and
derived-frame mapping before the next controlled Host candidate.

The operator also reported DISPLAY2 navigation showing only one image. Host
thumbnail sends continue 763→791 across four summaries, with zero failures. A
Client defect was found: per-display thumbnail revisions and the catalog revision
survived reconnection, rejecting a restarted Host's lower revisions. The repair
clears those session-scoped values on disconnect and automatic session rebuild,
discards pending navigation delivery on rebuild, and retains display/crop choices.
Same-session stale-frame rejection remains. An additive local diagnostic snapshot
reports selected display/revision and receive/publish/decode counters; no wire
protocol changed. Four panel cases (including two-display reconnect/render checks)
and six existing thumbnail worker/preparation cases pass. The Debug main build
through `build.ps1 -Configuration Debug -SkipConfigure -Target redclaw_desktop -NoPublish`
passes. The subsequent controlled Client deployment and live checks are recorded below.

Evidence under `build/reports/x00-t27/`: `p5-qsv-reason.json`,
`p5-navigation-live-result.json`, `p5-navigation-ui-tests.json`,
`p5-navigation-worker-tests.json`, and `p5-navigation-main-build.txt`.

### Controlled Client deployment and observed recovery

Fix `8c4fc09` was pushed before a clean Debug build/publication through `build.ps1`
in the existing independent checkout. The complete 336-file candidate and command
entry passed. Published SHA256 is
`3b02bc1fb35315dd14f5df3c32ff4df8b9273d7bedc04f87da02299dae2792f1`.
The first command-entry wrapper reported failure under Windows PowerShell although
help was emitted; the PS7 check confirmed exit 0 and the expected help text.

The first deployment exposed a launch-state mismatch: the old GUI command line
specified UDP 55000, while the live Client used 56000. Port 55000 was excluded by
Windows, so the candidate runtime could not start. The independent worker restored
the complete old publication; its original arguments had the same stale port.
After the worker exited, the original Client was relaunched with its previously
verified effective port 56000 and all other arguments preserved. It reconnected.
The second controlled deployment first checked effective and launch ports match;
new publication and the complete 330-file rollback both verify. Windows exclusions
were not modified. Future controlled upgrades must verify effective UI values
against launch arguments, including rollback arguments, before handoff.

The new Client reconnected through DHT/ICE and is streaming. Its initially selected
primary display is the peer's DISPLAY2. Four consecutive samples show thumbnail
revision 1761→1767→1772→1777, receive/publish counts 11→17→22→27, and zero thumbnail
decode failures. Main decoded surfaces advance 13→60 with zero decode/present
failures. This confirms current thumbnail delivery/display-state progress; the
same-GUI lower-revision Host restart regression is covered by the local two-display
test, not by restarting the remote Host for this check. No media protocol changed.

The remote Host was not replaced or restarted. Its independent four-minute recovery
guard completed with zero restart attempts and removed its scheduled task. The local
Client remains at the new publication. QSV GPU activation and matched performance
remain open; no new benchmark load was started. Evidence: `p5-navigation-pushed-build.txt`,
`p5-navigation-publish-validation.json`, `p5-navigation-deployment-recovery.json`,
`p5-navigation-deployed-manifests.json`, `p5-navigation-live-client-samples.json`, and
`p5-navigation-acceptance.json`. Both deployment attempts retain their own receipts.

## QSV surface-pool and mapping repair

The operator now requests local Debug Host / remote Client. The local Client has
already stopped. Complete this repair, push, build/publish clean pushed source,
then start the existing controlled Host entry with the retained DPAPI credential.
Wait for the operator's updated Client and picture confirmation before remote
sampling. This role change requires a new matched baseline.

Source inspection of the linked FFmpeg 8.1 implementation found two concrete
bridge defects: the QSV child pool used exact visible dimensions without decoder
binding, and the derived QSV context was fed through `av_hwframe_transfer_data`,
which rejects derived hardware contexts. The pool now uses a 32-slice,
16-aligned NV12 decoder texture. Its visible crop and codec dimensions remain
unchanged (1584×990 uses 1584×992 allocation); QSV surface metadata retains the
nominal frame rate. D3D11 pool frames are mapped into the derived QSV context,
retaining each source lease until the encoder releases it. There is no CPU
readback in this bridge. Allocation failure now includes HRESULT, dimensions,
format, slices and binding flags. The generation-latched CPU fallback and IDR
recovery remain available; no online protocol changed.

Both focused CTest suites passed (GPU ownership/policy and encoder execution).
Five developer GPU cases passed on the local NVIDIA device, including actual
allocation/copy into slice 31 of the aligned NV12 pool and checking the last
visible luma/chroma rows. Real NVENC encode/decode, resize/fallback/recovery and
WGC/DDA capture passed. Each capture backend completed three generations and
18 GPU-only frames without main-video readback increments. The Debug main build
passed through `build.ps1 -Configuration Debug -SkipConfigure -Target redclaw_desktop -NoPublish`.

On Intel HD Graphics 530 the decoder-target array produced a real bitstream with
zero CPU fallback, but software decode was black: the driver encoded slice 0
while the pool filled a later slice. Each QSV surface is now its own
render-target NV12 texture. The same developer-invoked gate then passed
(1584×990, 1504×938, and a replaced device) with decoded center and near-bottom
red above 210. That run does not measure latency.

The gate was first compiled here and not run on the earlier NVIDIA-only machine:

```powershell
build/ninja-x64/tests/Debug/redclaw_capture_gpu_input_tests.exe --gtest_filter=QsvInputHardware.*
```

Evidence under `build/reports/x00-t27/`: `p5-qsv-fix-focused-build.txt`,
`p5-qsv-fix-ctest.txt`, `p5-qsv-fix-local-gpu.json` and
`p5-qsv-fix-main-build.txt`. Linked FFmpeg source paths inspected:
`libavutil/hwcontext.c`, `hwcontext_d3d11va.c`, `hwcontext_qsv.c` and
`libavcodec/qsvenc.c`. Intel execution, peer connection and matched performance
remain separate acceptance items.

### Pushed Debug publication and local Host readiness

Repair `e1c915a` was pushed to `origin/main`, then built/published with
`build.ps1 -Configuration Debug -Target redclaw_desktop` from the clean existing
independent checkout. All 336 candidate files, required dependencies, build/hash
correspondence and the command entry passed. The formal `release/Debug` now
contains this candidate; the complete previous 336-file publication is retained
as a verified rollback. No runtime was running during replacement. The existing
supervisor startup entry and an independent rollback launch script are available.
Executable SHA256:
`d690dfd29910204bf049da21e530a573cdad81d4a117471e6d2feda586201276`.

The controlled local Host reuses the protected credential, previous capability
scope and effective ICE UDP port 56000 (binding verified before startup).
GUI and runtime executable hashes both match the candidate. At the readiness
check, runtime is running, DHT is reachable, listener is ready, phase is
`dht_waiting`, negotiation is `offer_ready`, and generation 1 has one successful
publication. No error, Client connection, captured frames or transmitted frames
are reported. Wait for the operator's remote Client update/connection and real
picture confirmation. No remote load or performance sampling has started.

Evidence: `p5-qsv-fix-pushed-build.txt`, `p5-qsv-fix-publication.json`, candidate
and rollback manifests, and `p5-qsv-fix-host-ready.json`. This readiness does not
qualify Intel QSV; the local Host uses NVIDIA hardware.

## Matched local Host performance acceptance

**X00-T27 P5: implementation/local functional checks passed; local NVENC
readback/CPU improvement confirmed; FPS/latency acceptance failed.** This run
does not measure Intel QSV performance. The peer's strict Intel functional gate
above remains passed. Diagnose the GPU preparation/encoding regression before
treating P5 as complete or advancing to the separate X00-T28 pacer change.

### Identity, rollout and fixed conditions

The operator requested local controlled Debug Host / remote Client and confirmed
normal video after each replacement, holding the Client viewport unchanged.
The CPU reference is the pushed P4 revision `af637da2a0efc6a318193be2db741e57c8f44f46`
(330 files, executable SHA256
`0f69d712419a95e2bbdb30db1c4ee5ce519da75fc1a02ba0eb1e8251a2a4df34`).
The GPU candidate is pushed revision `f5dac5d13e2d606a754578eb2b889d1a23410d81`
(336 files, executable SHA256
`b1ef1bf788859f1d41c8fb79c5b10e1a8055a226089d23458924eb93a62f5bbb`).
The existing verified P4 build was reused; f5dac5d was built and published from
clean pushed source through `build.ps1 -Configuration Debug -Target redclaw_desktop`.
Two focused CTest suites and five GPU/real-capture cases passed on this NVIDIA
device. Full candidate/dependency/hash and command-entry checks passed.

Both replacements used the existing controlled deployment entry with a complete
rollback and an independent recovery worker. Protected credentials, capability
scope and effective ICE port were retained; DHT publication, channels and real
media were checked before measurement. The final Host remains f5dac5d in
`release/Debug`. No formal release was published.

| Condition | Both groups |
| --- | --- |
| Host | NVIDIA GeForce GTX 1660 SUPER, DDA, NVENC |
| Capture / encode / Client viewport | 1920×1080 / 1778×1000 / 1920×1001 |
| Resolution policy | `stream_video_max_width=0`; existing viewport fitting retained |
| Encoder | 30 nominal FPS, time base 1/30, GOP 60 frames, effective configured 4267 kbps |
| Scene | Seed 2700, absolute-frame-reflection-v1, 28 sprites, fullscreen; source measured 21.32–21.33 FPS |
| Scene coordinates | 1536×864 logical pixels under existing desktop DPI; physical capture remains 1920×1080 |
| Logs / diagnostics | Main log visible, mirror hidden; 10000 ms diagnostics; DynamicLog requests 80 lines/s |
| Windows | Static, Dynamic, DynamicLog; each 10 s warmup then 60 s trace, three repeats |

Trace arming waits for the next diagnostic tick; the scene remains active through
drain. CPU was measured first, GPU second; this is a revision comparison, not a
randomized single-flag experiment. Network settings and adaptive policy were
unchanged, but their outputs were not frozen. Dynamic per-window median pacing
rates were CPU 5298/5298/5298 versus GPU 4870/3524/3524 kbps; DynamicLog was CPU
5298 versus GPU 3688 kbps. Adaptive target FPS settled at CPU 30 versus GPU 21–22.
These differences limit isolated attribution of sender tails; configured codec
bitrate stayed 4267 kbps. The slower local preparation/encode measurements still
require investigation. No bitrate, bursts, congestion, priorities or queues were
changed for this acceptance.

### Three-run results

Values are median [minimum–maximum] across three repetitions. CPU percentage is
Host runtime process time, with **100% equal to one logical core**; it is not
whole-machine utilization and was measured at the different achieved frame rates.
Frame age is local capture-ready to last send, not end-to-end display latency.
Preparation and total encoding are per-attempt weighted diagnostic means; total
encoding includes preparation, so these durations must not be added together.

| Dynamic metric | P4 CPU | P5 GPU | Median change |
| --- | --- | --- | --- |
| Sent FPS | 21.30 [21.30–21.33] | 15.78 [15.28–15.87] | −25.9% |
| Host CPU, one-core % | 53.28 [53.18–53.49] | 10.53 [10.39–11.00] | −80.2% |
| Capture copy, ms/frame | 7.47 [7.42–7.53] | 4.23 [4.02–4.33] | −43.4% |
| Input preparation, ms/attempt | 11.48 [11.38–11.64] | 29.63 [29.40–30.24] | +158.1% |
| Total encoding, ms/attempt | 21.17 [21.06–21.38] | 57.61 [57.30–60.68] | +172.1% |
| Frame-age P95, ms | 64.44 [63.60–66.35] | 156.15 [152.56–163.25] | +142.3% |
| Frame-age P99, ms | 76.04 [73.50–85.88] | 180.26 [180.19–191.97] | +137.0% |

| DynamicLog metric | P4 CPU | P5 GPU | Median change |
| --- | --- | --- | --- |
| Sent FPS | 21.32 [21.30–21.33] | 15.55 [15.50–15.80] | −27.1% |
| Host CPU, one-core % | 51.89 [51.32–52.12] | 10.28 [9.51–11.29] | −80.2% |
| Capture copy, ms/frame | 7.42 [7.41–7.47] | 4.17 [4.13–4.30] | −43.8% |
| Input preparation, ms/attempt | 11.39 [11.37–11.43] | 29.81 [29.34–30.62] | +161.8% |
| Total encoding, ms/attempt | 21.07 [21.04–21.08] | 58.44 [58.05–60.18] | +177.3% |
| Frame-age P95, ms | 64.46 [63.72–65.66] | 156.62 [152.27–157.56] | +143.0% |
| Frame-age P99, ms | 73.18 [70.69–77.44] | 184.35 [182.84–186.66] | +151.9% |

Every range in these tables is disjoint. Both static groups legitimately sent
zero new frames; their frame quantiles are unavailable. Static Host CPU ranges
overlap (CPU 3.45–4.91%, GPU 3.63–4.62%): improvement is unconfirmed. This scene's
21.3 FPS source is not a measurement of maximum system capacity.

All 18 windows completed: CPU 7673 sent traced frames, GPU 5627, no unsent
outcomes, no Host/GUI trace overflow, and no sampled capture/encode/transmit
failure increments. GPU DynamicLog-2 frame 7315 lacks capture timestamps while
encoding/sending timestamps are present. It is retained for output FPS and
excluded from capture/frame-age distributions (5626 GPU frames have complete
capture timing). The strict whole-timeline analyzer rejected that row; the
separate audit accounts all 7673 CPU and 5626 complete GPU rows without modifying
raw traces. Do not report the GPU strict full-trace check as passed.

In the within-window diagnostic counter spans (about 40 s, not the full 60 s),
each dynamic CPU window adds 852–853 main-video readbacks and CPU copies. Every
GPU window adds zero of either, reports GPU input confirmed, and retains zero
fallbacks. GPU native texture copies remain: this is not zero-copy. Both modes
add zero large CPU buffer allocations or pool exhaustion after warmup. GPU
thumbnail delivery advances 38–39 per dynamic counter span, with zero failures;
each thumbnail reads only 230400 bytes (320×180 BGRA). Hidden mirror refresh
deltas stay zero. These local Host logs do not measure remote Controller GUI
optimization benefit or requalify the peer's previous DISPLAY2 scenario.

### Five-minute continuous run and limits

The same GPU Host ran a continuous dynamic scene for 301.93 s of status sampling,
sending 4842 real frames (16.04 FPS), with no capture/encode/transmit failures,
synthetic frames or reconnects. Across the 300.01 s diagnostic span, main-video
readbacks, CPU copies, large allocations, pool exhaustion, capture rebuilds,
backend switches and recovery reset/success increments were all zero. GPU input
remained confirmed with zero fallbacks. Thumbnail sends/readbacks advanced 291
with zero failures, totaling 67046400 readback bytes.

Two bounded 60 s trace segments contain 959 and 905 sent frames, zero overflow
and valid send accounting. Frame-age P95/P99 moved from 129.51/161.02 to
141.21/177.28 ms; this does not show continuous unbounded growth, but neither
does it resolve the tail regression. The recorder did not trace all five minutes.
Host working set stayed 60.85–62.67 MiB and private bytes 484.89–486.02 MiB;
first/last 30 s private-memory medians were 486.012/486.016 MiB. These process
figures are not total GPU memory or a general leak-free guarantee.

The operator confirmed normal decoded pictures after both replacements. Detailed
remote text/line/color/cursor comparison is not yet confirmed; no peer pixel
capture or objective image comparison was collected. Existing real encode/decode,
crop/color/cursor and recovery tests remain functional evidence only. Current
timings localize the principal new cost to preparation/encoding, but do not yet
separate mutex wait, video-processor work, surface lifetime/copy or NVENC submit.
That separation is the next P5 investigation; pacer policy is unchanged.

### Retained evidence and recomputation

All raw artifacts are local under `build/reports/x00-t27/`, excluded from Git:

- `p5-cpu-reference-20260929/`, `p5-gpu-candidate-20260929/`: plans, identities,
  scene reports, per-window status/CPU/memory, geometry, original traces and logs.
- `p5-matched-comparison.json`, `p5-rate-context.json`, `p5-trace-audit.json`:
  three-run ranges, configuration/adaptation context and exact missing-row scope.
- `p5-gpu-soak-20260929/`: result, 142 samples, two original traces and analysis.
  The short tail-only diagnostic extract is retained; `host-stage-complete.txt`
  recovers the complete original log summaries and is used for soak counters.
- `p5-acceptance-ctest.txt`, `p5-acceptance-gpu.json`,
  `p5-acceptance-pushed-build.txt`, candidate/deployment manifests and receipts.

From the repository root, the retained local scripts recompute the tables:

```powershell
python build/reports/x00-t27/analyze_baseline.py build/reports/x00-t27/p5-cpu-reference-20260929
python build/reports/x00-t27/analyze_baseline.py build/reports/x00-t27/p5-gpu-candidate-20260929
python build/reports/x00-t27/compare_p5.py
python build/reports/x00-t27/audit_p5_traces.py
python build/reports/x00-t27/analyze_p5_soak.py
```

The audit and soak scripts reuse `scripts/capture/analyze-video-link-baseline.py`
for nearest-rank distributions and send accounting. Diagnostic means have
explicit capture/attempt denominators and may straddle status boundaries.
No uncalibrated peer timestamps are subtracted. This acceptance changes only
documentation and local measurement artifacts, not production behavior.

## DDA shared-device wait diagnosis

**The principal local NVIDIA regression is shared-device synchronization during
`AcquireNextFrame(50)`.** The capture thread waits outside our recursive mutex,
but on this DDA/driver path the wait occupies the device's internal
`ID3D11Multithread` protection. GPU input shares that device with capture, so
video-context calls and encoder input submission can wait for desktop acquisition.
Moving acquisition outside the application mutex was therefore insufficient.
The CPU-input branch does not attach the capture device as the encoder's hardware
context, which explains why eliminating CPU readback exposed this coupling.

### Controlled local evidence

Added cumulative preparation sub-stage wall clocks, with a local v1 diagnostic
contract and no per-frame disk output. They separate the application mutex,
pipeline setup, input-view creation, video-context state setters, blit submission,
frame release/pool/map and copy lock/submission. These are CPU-observed call
durations, not GPU execution timestamps. Existing codec send/receive timers remain.
No media/control protocol, capture wait policy, bitrate, frame size or runtime
synchronization policy changed.

The developer-only `GpuInputProfile` probe uses real DDA 1920×1080 capture,
1778×1000 NVENC encoding, 30 nominal FPS / 60-frame GOP and 4267 kbps. It uses the
same seed-2700 moving desktop, warms for 3 s, measures about 8 s and repeats each
case three times. Concurrent cases have one latest pending frame and the existing
three-slot capture pool. GPU input stays confirmed with zero fallback. The
existing Host remains running as background load in every case; probes do not
send network media. These are mechanism comparisons, not replacements for the
earlier cross-LAN acceptance.

| Per-attempt ms, median [three-run range] | Serial capture then encode, 50 ms timeout | Concurrent capture, 50 ms timeout | Concurrent capture, 0 ms timeout |
| --- | --- | --- | --- |
| Input preparation | 0.038 [0.037–0.041] | 35.737 [33.782–36.279] | 0.047 [0.047–0.052] |
| Application scale-lock wait | <0.001 | 0.025 [0.019–0.027] | <0.001 |
| Video-context state setters | 0.002 [0.002–0.002] | 30.486 [29.019–30.595] | 0.008 [0.008–0.010] |
| VideoProcessorBlt call | 0.001 [0.001–0.002] | 0.004 [0.004–0.390] | 0.002 [0.002–0.002] |
| Encoder-surface copy call | <0.001 | 4.940 [4.567–5.200] | 0.001 [0.001–0.001] |
| `avcodec_send_frame` | 13.434 [12.737–13.916] | 53.400 [51.002–55.534] | 9.706 [9.261–9.939] |
| Codec output FPS | 21.427 [21.395–21.436] | 11.336 [10.796–11.469] | 21.439 [21.369–21.461] |

Frame release and pool lease costs are about 0.002 ms each in all cases. The
large preparation cost is not in our mutex or the measured blit submission;
it appears in calls that need the protected device. The driver may defer GPU
work, so a small blit call duration does not measure shader execution cost.

A second probe removes scaling and encoding entirely. While a capture worker
acquires frames, the probe obtains our mutex first, then times only
`ID3D11Multithread::Enter()` / `Leave()`. Thus application-mutex wait and actual
encoder work are excluded. Three 5 s repetitions produce:

| Protected-device entry | DDA 50 ms acquisition | DDA 0 ms acquisition |
| --- | --- | --- |
| Mean ms, three-run median [range] | 32.135 [31.174–32.402] | 0.0044 [0.0037–0.0051] |
| P95 ms, three-run median [range] | 47.134 [47.016–48.366] | 0.036 [0.012–0.039] |

This directly confirms internal protected-device contention on the local driver,
independent of encoder complexity, input-view allocation, network pacing or
frame-pool release. The 0 ms comparison uses a 1 ms requested sleep after an
empty poll; its actual wake cadence is controlled by the OS. It is a diagnostic
contrast, not a production scheduling recommendation or a CPU/power acceptance.

The linked FFmpeg NVENC source routes D3D11 frames through resource registration,
mapping and encode submission within the existing send call. This run did not
instrument proprietary `nvEnc*` calls separately; their exact internal split and
the residual 9–14 ms submission cost remain unqualified. Intel/QSV and WGC have
not been measured by these DDA-specific probes.

### Verification and next change

The 9 stage-profile runs and 6 device-entry runs passed with zero capture failures
and no observed GPU fallback. The two directly affected CTest suites and three
existing GPU conversion/real-codec/fallback cases passed. The main program passed
`build.ps1 -Configuration Debug -SkipConfigure -Target redclaw_desktop -NoPublish`.
The first CTest shell lacked `ctest` on PATH and ran no tests; using the configured
VS2022 executable completed the check. Both diagnostic suites are excluded from
the unattended CTest filter and print a versioned JSON record via GoogleTest.

Next within P5: change DDA scheduling so long waits do not occupy the device
shared by capture and encoder, while preserving multithread protection, bounded
ownership, stop/recovery behavior and reasonable idle CPU/wakeup cost. Measure
short/bounded acquisition and waiting outside that device before choosing a
runtime policy. Do not disable D3D11 protection, add an unbounded queue or infer
that zero-timeout polling is already suitable for deployment. After a fix, follow
push → clean Debug publication → controlled Host replacement → operator picture
confirmation, then repeat the affected matched acceptance. P5 remains open.

Local evidence: `build/reports/x00-t27/p5-input-profile-20260929/`,
`p5-device-entry-profile-20260929/`, `p5-input-diagnosis.json`,
`p5-profile-focused-build.txt`, `p5-profile-main-build.txt`, `p5-profile-ctest.txt`
and `p5-profile-hardware.json`. Reproduce explicitly with a moving desktop:

```powershell
build/ninja-x64/tests/Debug/redclaw_capture_gpu_input_tests.exe --gtest_filter=LocalDiagnostic/GpuInputProfile.* --gtest_output=json:build/reports/gpu-input-profile.json
build/ninja-x64/tests/Debug/redclaw_capture_gpu_input_tests.exe --gtest_filter=LocalDiagnostic/DdaDeviceWaitProfile.* --gtest_output=json:build/reports/d3d11-entry-profile.json
```

## Bounded DDA wait repair

DDA now calls `AcquireNextFrame(0)` and, after an empty result, waits up to 8 ms
outside the capture device/context locks before probing again. One steady-clock
deadline preserves the caller's original total wait budget (50 ms in streaming).
Late OS wakeups do not restart that budget. Success and device loss return
immediately; a zero budget makes exactly one probe. Multithread protection,
shared device ownership, bounded frame slots and codec settings are retained.
There is no new thread, timer-resolution change or network policy change.

Local cumulative diagnostic v1 adds DDA probe and external-wait counts to the
existing ten-second summary. They count API calls and requested waits, not OS
context switches or system-wide wakeups. No per-frame disk output is added.

### Local before/after validation

The existing concurrent real-DDA/NVENC probe was rebuilt before changing runtime
acquisition, then repeated after the repair. Each group uses three seed-2700
dynamic desktop runs, physical 1920×1080 capture, 1778×1000 encode, 30 nominal FPS,
GOP 60 and 4267 kbps. Stage measurements exclude the first 3 s and cover about
8 s; process CPU covers the whole approximately 11 s probe, including warmup.
The old f5dac5d Host remains background load throughout; these probes send no
network media. The desktop generator achieves about 21.3 FPS in this setup.

| Dynamic metric, median [three-run range] | Before repair | After repair |
| --- | --- | --- |
| Codec output FPS | 10.49 [9.67–11.17] | 21.28 [21.18–21.32] |
| Input preparation, ms/attempt | 33.762 [32.246–34.586] | 0.054 [0.052–0.057] |
| Codec submission, ms/attempt | 60.635 [56.653–68.341] | 10.474 [10.221–10.802] |
| Total encoding, ms/attempt | 94.451 [88.947–102.976] | 10.575 [10.325–10.898] |
| Probe process CPU, % of one logical core | 5.21 [3.69–5.51] | 4.38 [4.25–4.95] |
| Stop request to producer join, ms | 0.71 [0.51–16.12] | 8.04 [0.50–49.97] |

The repaired dynamic runs make approximately 79 DXGI probes and 58 external
waits per second. Three separate 5 s static-desktop runs show CPU ranges
0.31–1.87% before and 0.31–0.94% after, with about 66 probes/waits per second
after repair. Both dynamic and static CPU ranges overlap: CPU improvement is
**unconfirmed**. These are short process measurements, not power measurements.
Call-rate counters are collected after producer join and divided by the active
window, so may include up to one final bounded acquisition (at most approximately
1% of a static window). Static stop-join maximum is 47.14 ms; cancellation still
waits for the current acquisition budget and OS scheduling, rather than being
instantaneous. No unbounded polling or new stop/recovery failure was observed.

Three focused automatic CTest suites pass, including four fake-clock tests for
zero budget, total deadline, success/device-loss and late wakeups. Seven manual
hardware cases pass without skips: conversion/crop/color, padded NV12 surfaces,
actual codec/fallback/resize, real WGC and DDA across capture generations,
capture-device recreation with a live encoder, and GPU/CPU cursor comparison.
The local real codec remains NVENC; the prior Intel QSV functional gate is retained
as separate evidence. Intel performance is not inferred from this repair.
The main program passes `build.ps1 -Configuration Debug -SkipConfigure -Target
redclaw_desktop -NoPublish`. The first restricted-environment build stalled in
Ninja without compiler children/output and was stopped; the normal desktop
environment retry passed. No running Host was stopped during that recovery.

The local stage regression is repaired in these probes. Cross-LAN frame-age tails,
adaptive submission and decoded visual quality still require the pushed candidate,
controlled Host replacement and operator reconnection confirmation. P5 remains
open until that affected acceptance is complete.

Local artifacts: `build/reports/x00-t27/p5-wait-{before,after}-{dynamic,static}/`,
`p5-wait-hardware/`, `p5-wait-comparison.json`, `p5-wait-ctest.txt`,
`p5-wait-focused-build.txt` and `p5-wait-main-build.txt`. Recompute medians/ranges
with `python build/reports/x00-t27/compare_p5_wait.py`.

### Pushed candidate and controlled Host readiness

Pushed implementation `a656edeeb58901bf4e60f9cad1b7811aa1f4e56f`, then built the
clean independent checkout through `build.ps1 -Configuration Debug -Target
redclaw_desktop` and published all 336 files. Published/build executables match;
the command-entry check passes. SHA256:
`de340ee7859f233e9a8c4d9fea9a8b033d8c1d4bdb8b391338803b00f554cd39`.

The first independent upgrade timed out during pending-directory verification
before handoff; the old Host was not stopped and remained connected. A separate
full 336-file hash check passed in 50.70 s. A new operation repeated the complete
checks and succeeded, retaining the failed receipt. Formal and rollback 336-file
manifests both verify. No check was skipped and no timeout was relaxed.

The local controlled Debug Host now runs the candidate from `release/Debug` with
existing protected credentials and effective/launch ICE port 56000 retained.
Fresh status shows DHT listener ready, reachable and published, phase
`dht_waiting`, no connected Client and no captured/transmitted media yet. The
operator has been asked to reconnect and confirm normal picture/unchanged
1920×1001 viewport. No candidate cross-LAN benchmark load or sampling has begun;
P5's deployed FPS/latency improvement remains pending that confirmation and the
affected matched comparison. The prior Intel functional evidence is unchanged.

Receipts: `p5-wait-pushed-build.txt`, `p5-wait-candidate.json`,
`p5-wait-candidate-manifest.json`, `p5-acceptance-wait-deployment/` and
`p5-acceptance-wait-deployed.json` under the same local evidence root. The
`p5-wait-pending-measurement-plan.json` retains false operator-confirmation flags
and `executed=false`; it is preparation only.
