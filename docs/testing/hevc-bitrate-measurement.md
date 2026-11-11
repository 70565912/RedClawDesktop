# HEVC versus H.264 bitrate measurement

Decision: [D8](../../DECISIONS.md#d8--hevc-is-preferred-when-both-peers-have-hardware-hevc-h264-stays-the-fallback). Goal: lower bitrate at comparable picture quality. This page is the procedure. It records no numbers. Nothing here has been run on Ryan's machines yet.

## What the build does

- Host HEVC target and floor = 70% of the H.264 interactive budget for the same pixels and FPS (`kHevcBitratePercentOfH264` in `src/capture/include/redclaw/capture/capture_module.h`). The 400 kbps absolute floor and the 1.2x max/target ratio are unchanged.
- 70% is a planning value, not a measurement. It sits at the conservative end of the 25–40% savings usually quoted for HEVC Main over H.264 High at equal quality. Desktop content (text, flat UI) and the ultra-low-latency presets used here (no B-frames, no lookahead) can save less, so the value starts conservative and should only be lowered after this measurement.
- Worked values from `test_hevc_budget_is_lower_than_h264_for_the_same_pixels`: 1920x1080 at 30 FPS is 4976 kbps for H.264 and 3483 kbps for HEVC. The interactive floor for 1680x1050 at 30 FPS is 2963 kbps for HEVC.
- Because the rate controller is given the lower target, a wire bitrate near 70% of H.264 is expected by construction. **What the measurement decides is whether picture quality stays comparable at that bitrate**, plus the latency, CPU/GPU, and keyframe-size side effects.

## Procedure

Run on the same two machines, the same Host display geometry, the same network path, and the same scene for both codecs. Use Debug builds from this branch on both sides so that `Runtime video codec negotiated` lines exist.

1. Start the Host and Controller with the H.264 rollback switch on the **Host**: `--stream-video-codec h264` (or profile key `stream_video_codec=h264`, or the GUI "Video codec: H.264 only (rollback)" selector). Confirm the Host log shows `Runtime video codec negotiated role=host codec=h264 reason=forced_h264_by_setting`.
2. Run the existing Host baseline into a new directory:

   ```powershell
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/capture/run-video-link-baseline.ps1 `
     -RunDirectory <host-run-dir> -ContractFile <contract.json> -ReportDirectory build/reports/hevc-ab/h264
   python scripts/capture/analyze-video-link-baseline.py build/reports/hevc-ab/h264
   ```

3. Restart the Host with `--stream-video-codec auto` (the default). Confirm `codec=hevc reason=auto_hevc_hardware_on_both_sides`. If the line says `codec=h264`, the `reason=` field explains why (`peer_no_video_codec_report`, `peer_no_hevc_decoder`, `peer_hevc_decode_not_hardware`, `host_no_hevc_hardware_encoder`, `host_hevc_encoder_open_failed`). Stop there and record it; that machine pair cannot run the A/B.
4. Run the baseline again with the same contract file into `build/reports/hevc-ab/hevc` and analyze it.
5. Compare:

   ```powershell
   python scripts/capture/compare-video-codec-baseline.py build/reports/hevc-ab/h264 build/reports/hevc-ab/hevc
   ```

   The comparator exits non-zero and lists problems when a report contains the wrong codec, a mid-run renegotiation, or a different contract. Otherwise it writes `codec-comparison.json` with per-scene medians and HEVC/H.264 ratios for wire kbps, keyframe and ordinary frame wire bytes, sent FPS, encode p95, frame-age p95, and Host CPU.
6. Picture quality. The comparator does not judge it. For each scene, take Controller screenshots at the same moments in both runs (static text, scrolling text, moving window) and compare them side by side. Record whether text is equally legible and whether motion shows more blocking or smearing under HEVC.
7. Repeat once on the remote (DHT/ICE) path if available.

Keep generated reports local; they contain endpoint data. Record the summary (ratios, quality judgment, machine/GPU/driver names) in [PROGRESS.md](../../PROGRESS.md) and, if the 70% value changes, in D8.

## Pass / adjust rules

- Comparable quality and latency at about 0.70 wire kbps: keep 70%.
- Visibly worse quality: raise `kHevcBitratePercentOfH264` (for example to 80%) and re-run.
- Same quality with headroom: a lower value can be tried, one step at a time, with a re-run.
- HEVC encode p95, frame-age p95, or Host CPU clearly worse than H.264: record it; the `h264` rollback stays available while it is investigated.

## Automated checks that already run

- `redclaw_video_codec_negotiation_tests`: selection rules, old-peer fallback, rollback, bounded wait, wire round-trip.
- `redclaw_capture_encoder_low_latency_profile_tests`: HEVC budget below H.264 for the same pixels, 400 kbps floor kept.
- `redclaw_capture_encoder_backend_bridge_tests`: automatic HEVC needs an HEVC hardware encoder and never selects libx265.
- `FfmpegRoundtripIntegration.HevcSoftwareRecoveryProducesStandaloneIdr` (integration, libx265): a requested HEVC keyframe is an IDR (NAL 19/20) that a fresh decoder can show.
- `FfmpegHevcHardwareRecoveryIntegration.RequestProducesStandaloneIdr`: the same check on the machine's HEVC hardware encoder. Excluded from CTest like the NVENC check; run it by hand on each machine:

  ```powershell
  build/vs2022-x64/tests/Debug/redclaw_ffmpeg_roundtrip_integration_tests.exe --gtest_filter=FfmpegHevcHardwareRecoveryIntegration.*
  ```

- `scripts/capture/test-compare-video-codec-baseline.py`: offline checks for the comparator.
