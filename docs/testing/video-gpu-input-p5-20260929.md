# X00-T27 P5: synchronized GPU input

P5 is implemented and locally validated. WGC and DDA each completed three capture
device generations through the real encoder; **36 confirmed GPU-only frames had
zero additional main-video CPU readbacks**. This local device uses `h264_nvenc`.
The current remote Host uses QSV, so its GPU activation, performance comparison
and final five-minute visual-quality acceptance remain separate gates.

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
