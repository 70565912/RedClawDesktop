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
