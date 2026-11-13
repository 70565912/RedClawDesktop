# Progress

As of 2026-11-12 (Asia/Shanghai). The clipboard and mainline sections were written 2026-11-06.

`main` includes the clipboard paste fix at `c8ec2a2` (PR #5). Product version metadata is 0.1.12 so the Windows publish script can run after this preparation merges. That script has not been run. The published GitHub package remains v0.1.11, and there is no v0.1.12 tag.

Intent and acceptance: [SPEC.md](SPEC.md). Decisions: [DECISIONS.md](DECISIONS.md). Agent gate: [AGENTS.md](AGENTS.md).

## 2026-11-10 — Runtime logs go to files (M10-T02)

- Remove the runtime log card and its copy and clear actions from the connection pages. Remove the diagnostic log displays from the workspace. Connection progress and actionable errors remain visible. The existing diagnostic writer still records GUI and runtime messages in the rotating local files. See [D9](DECISIONS.md#d9--show-connection-status-and-write-runtime-logs-to-files).
- The Debug application built through `build.ps1 -SkipConfigure -Target redclaw_desktop -NoPublish`. An isolated GUI run returned home and exited successfully. The home and Host waiting snapshots show the connection controls with no log card, and the GUI diagnostic marker appears in the file log. Evidence is under `build/reports/ui-file-log-20261110`.
- With a workspace-local test temporary directory, all 42 focused connection, workspace, and diagnostic writer cases passed. The D3D11, file logger, and structured redaction CTest suites also passed. The broader UI run in the default environment hit filesystem and Windows credential or registry restrictions. The running Host was not replaced.

- The independent Debug candidate passed a 345-file manifest check and a restricted-PATH startup probe. CIM could not verify the running Host from the build environment, so the maintenance worker did not take ownership. The existing Host continued DHT publication in standby.

## Clipboard paste fix, on main

Squash-merged as `c8ec2a2` (PR #5), from `ce0990fd`. Decision: [D7](DECISIONS.md#d7--capability-5-clipboard-requests-always-end-visibly). [Release notes](docs/releases/v0.1.12.md).

- Root cause. With capability 5, the Host injects the fixed Ctrl+V right after it installs the offer, before any bulk transfer exists. Injection went through `RemoteInputSession::paste_verified_clipboard`, which only works while a transfer gate is held. So every Host shortcut paste failed with `clipboard_input_revoked`. The Controller only showed the error in the hidden file panel. That is why no four-key paste inject reached the Host.
- Contributing liveness defects, all silent before:
  - A stopped native port (listener failure, queue overflow, exception) stayed dead for the whole session.
  - Clipboard capture and `OleSetClipboard` were not retried while another program held the clipboard open, so the Host kept a stale `RedClaw.ClipboardOffer.v5`.
  - A Host demand with a stale token, or one that hit a busy slot, was dropped without a reply.
  - Unanswered reservations and shortcut waits had no bound.
- Fix:
  - `paste_shortcut_clipboard` injects the shortcut paste. It is bound to the eligibility revision and refused while a transfer gate is held.
  - Every request the session cannot serve gets a `kCancel` with an error code.
  - A text or image reservation with no peer reply is released after 15 s, which ends a blocked `GetData`.
  - A shortcut capture that does not answer fails after 5 s.
  - Failed offer sends are retried.
  - A dead native port restarts with a 1 s to 30 s backoff.
  - The native port retries capture, install, and listener registration.
  - The Controller UI shows every refusal.
  - Runtime lines starting `Runtime clipboard role=` and the GUI line `Clipboard shortcut intercepted` log state changes, never payloads.
- Automated: `redclaw_clipboard_demand_liveness_tests` (11 cases) and `RemoteInputSessionTests.ShortcutClipboardPasteNeedsNoTransferGateButStaysRevisionBound`. Both run in the unit CI. On Linux with a capability shim, all 11 liveness cases passed on the fix, and 10 failed against the previous session code. PR #5 CI jobs `build-and-test`, `hint`, and `docs-guard` passed (Actions runs [42169935054](https://github.com/70565912/RedClawDesktop/actions/runs/42169935054), [42169935102](https://github.com/70565912/RedClawDesktop/actions/runs/42169935102), and [42169935053](https://github.com/70565912/RedClawDesktop/actions/runs/42169935053)). Ryan confirmed bidirectional Controller-to-Host clipboard paste on live two-machine Debug builds of `ce0990fd`, now on main as `c8ec2a2`. The Windows Release portable package and its publish-step smoke are produced later and are not part of this preparation.
- Run locally: `cmake --build --preset debug-local --target redclaw_clipboard_demand_liveness_tests redclaw_remote_input_session_tests`, then `ctest --test-dir build/vs2022-x64 -C Debug --output-on-failure -R "clipboard_demand_liveness|remote_input_session"`.
- Procedure written with the fix:
  1. Copy text on the Controller. The Host log shows `event=offer_received`, and then `event=port offer_installed`.
  2. Focus a Host text field, take input control, and press Ctrl+V, right Ctrl+V, and Shift+Insert. The text appears on the Host. The Host log shows `event=paste_inject ... result=ok`.
  3. Use Host right-click paste. The text appears, or the app returns within 15 s.
  4. Any failure is shown in the Controller file/clipboard panel with a reason.
- Rollback: revert `c8ec2a2`, or redeploy the previous Debug binaries on both machines. The wire messages are unchanged, so mixed old and new builds still connect. Shortcut paste needs the new Host build. With an old Host it still fails as before, but a new Controller times out and shows the failure.

## HEVC preferred codec, on a branch

Decision: [D10](DECISIONS.md#d10--hevc-is-preferred-when-both-peers-have-hardware-hevc-h264-stays-the-fallback) (Ryan, 2026-11-11). Branch `cursor/hevc-preferred-codec-5649`, pull request against `main`, not merged. Ryan has run two rounds on his machines; the latest build on the branch has not been run there yet.

- Wire: optional `StreamControlMessageV1` fields 92–94 (`video_codec_version`, `video_decode_codecs`, `video_hardware_decode_codecs`; bit 0 H.264, bit 1 HEVC). Missing fields mean an older peer, which gets H.264.
- Controller: probes FFmpeg decoders and D3D11VA H.264/HEVC decode profiles at startup and advertises them on every Hello/Capabilities. Log: `Runtime video decode capabilities`.
- Host: waits up to the existing 3 s viewport grace for the report, then selects the codec (`HostVideoCodecNegotiatorV1`, `src/protocol/src/video_codec_negotiation.cpp`). Log: `Runtime video codec negotiated ... codec= reason=`. If the HEVC hardware encoder fails to open, the Host falls back to H.264 for the rest of the process. If a reconnecting peer cannot decode HEVC, it rebuilds as H.264 (`Runtime video codec renegotiation`). If a later report makes HEVC eligible, it rebuilds as HEVC (`from=h264 to=hevc`).
- Encoder: automatic HEVC never opens libx265. HEVC target and floor = 70% of H.264 (unmeasured). Requested HEVC keyframes are forced to IDR; on x265 this needs `open-gop=0` too, otherwise x265 emits CRA. The new integration test caught that.
- Rollback: `--stream-video-codec h264`, profile `stream_video_codec=h264`, or the GUI "Video codec" selector. Or redeploy the previous build on either side; mixed versions negotiate H.264.
- Checks run in the Linux authoring environment, with scratch builds against Ubuntu FFmpeg 6.1 (not MSVC):
  - `redclaw_video_codec_negotiation_tests`: 12 passed.
  - `stream_control_protocol_tests`: 24 passed.
  - Capture bridge and low-latency profile tests: passed.
  - `redclaw_ffmpeg_roundtrip_integration_tests` portable set, including the new libx265 IDR case: 5 passed.
  - `test-compare-video-codec-baseline.py`: 3 passed.
  - `src/main.cpp`, the GUI, and the Windows D3D11 probe were not compiled there; Windows CI on the pull request is the build check.
- Live two-machine test of the first build (Ryan): negotiation, fallbacks, reconnect, reverse direction and mixed versions passed. It found the HEVC send pacing cap stuck at the sent rate (about 1.3 Mbps against a 2.7 Mbps encoder target). Cause from code reading: probes need non-application-limited samples, which low-bitrate HEVC frames rarely produce. Fix on this branch: a demand floor on the cap (125% of target, or the encoder max), drained below only by queue, loss or RTT evidence and restored after a 2 s hold. Not yet rerun on Ryan's machines, so the live effect is unmeasured.
- Same follow-up: the Host encoder capability log now separates `compiled_hevc_*` from `probed_hevc_*` (real encoder open), and the baseline analysis skips leading zero-capture-time frames with a count. Details in [DEVLOG](docs/logs/DEVLOG.md).
- Retest of `41b60846` (Ryan): the pacing fix works. HEVC sent 0.72x the H.264 bitrate at equal FPS, and the cap never went below the floor.
- The same retest found a Controller problem on the Optimus laptop (Intel HD 530 drives the display, GTX 960M is the second GPU). The presenter and decoder shared one D3D11 device. GUI present preparation stalled for 440–475 ms, the 2-slot shared-memory ring fell behind, and the GUI sent 298 `shared_memory_sequence_gap` keyframe requests (6 under H.264). Changes on this branch, all independent of the codec:
  - The GUI decoder has its own D3D11 device on the presenter's adapter. Frames reach the presenter through three keyed-mutex shared textures (a GPU copy, not zero-copy). When every slot is busy, that frame is skipped for display only; decoding continues.
  - The frame ring has 6 slots (layout version 4). Gap keyframe requests are coalesced to at most one per 500 ms.
  - Presentation stops while the window is hidden or minimized or the display is off, and probes every 250 ms while occluded. Each D3D call in present preparation is timed, and calls over 50 ms are logged.
  - The presenter uses the adapter that owns the window's monitor, otherwise the first adapter with outputs. The HEVC decode probe uses the same choice. Adapter name, vendor and LUID are logged.
  - After 5 ring-overrun episodes within 30 s on HEVC with no network loss, the Controller stops advertising HEVC. The Host then renegotiates to H.264 and logs `reason=controller_hevc_decode_overload` (new optional wire field 95).
  - Linux checks: flow-control tests 12/12, codec negotiation 19/19, stream control 24/24. Windows-only code passed a MinGW syntax check only. The Windows build, the ring tests and the shared-texture test rely on CI. Nothing in this round has run on Ryan's machines yet.
- Adapter rule changed the same day (Ryan, 2026-11-11, after `c4fb829`): HEVC is probed only on the adapter that owns the window. If it cannot hardware-decode HEVC, no other adapter is tried and HEVC is left out of the decode report, so the Host sends H.264 (`peer_no_hevc_decoder`). The GUI passes the window adapter as `--present-adapter-luid`. Moving the window to another adapter's monitor does not renegotiate; restart the session. There is no cross-adapter copy. Linux: flow-control tests 20/20 and the negotiation test that covers the report. Not yet run on Ryan's machines. Details in [DEVLOG](docs/logs/DEVLOG.md).
- Shared-surface present on the same laptop (`b89a396e`, Intel HD 530): `CreateVideoProcessorInputView(shared)` returned `0x80070057` because the NV12 slot used `D3D11_BIND_SHADER_RESOURCE` only, and the GUI stayed on the CPU transfer path. The bridge now prefers render-target plus shader-resource, then render-target, then no bind flags, and keeps the first set whose video-processor input view succeeds. The 5 s stats line logs `shared_texture_bind`. A first-present `CreateVideoProcessor` of 53.7 ms stays over the 50 ms log line; the threshold is unchanged. Not yet rerun on the laptop.
- Codex review of `838bf86` (2026-11-12, Ryan approved all seven fixes):
  - A published shared decode slot is not overwritten until the presenter releases that frame. Before, a GUI stall could show newer pixels under the old frame id.
  - Frames discarded locally while waiting for an IDR no longer count as network loss, so the HEVC overload fallback can fire.
  - A shared-texture failure keeps decode on the display adapter with CPU transfer, instead of FFmpeg's default device.
  - Each session start rebuilds the presenter device if the window's adapter changed.
  - A running H.264 encoder is rebuilt as HEVC when a later peer report allows it.
  - Restoring a hidden, minimized, occluded or display-off window after skipped frames requests a keyframe.
  - HEVC takes 70% after the 20,000 kbps H.264 ceiling, so its maximum is 14,000 kbps.
  - Linux: flow-control 24/24, negotiation 21/21, encoder profile tests pass. Not yet run on Ryan's machines. Details in [DEVLOG](docs/logs/DEVLOG.md).
- Needs Ryan's machines: the on-device checklist in the pull request and [HEVC bitrate measurement](docs/testing/hevc-bitrate-measurement.md), including the laptop Controller checks for this round.

## 2026-11-12 — Pause Controller decode while presentation is paused (M02-T03)

Local follow-up at base `0c96a109` on `codex/pause-decode-while-hidden`. This run does not push, open a PR, merge, publish or start/stop a RedClaw application. The laptop display-off overload reports in the task are design inputs, not reproduced measurements.

- Implements review P1 items 1–6: independent visibility transitions/occlusion polling; GUI O(1) ring discard with local runtime eligibility; queued-request cancellation and recovery suspension; post-resume sequence/reference boundary with IDR-submission/output distinction; optional feedback-validity intervals; one rearmed HEVC hardware trial under amended D10.
- Local ring layout is v5; network fields 96–100 are optional v1 feedback validity. Updated Hosts exclude paused/recovering/transition-spanning samples. Older Hosts retain legacy adaptation and may temporarily reduce FPS on wake. Sender pause/throttle remains deferred to 4.4; only a typed local hook exists.
- D3D11 covers display power, hide, minimize and DXGI occlusion. Qt/OpenGL cover hide/minimize explicitly; display-power and full-occlusion detection for those fallback presenters are excluded.
- Validation passed locally with VS2022 VsDevCmd imported (x64 host/target), after a fresh CMake configure, correction of the generated Ninja/MSVC Chinese include prefix and clean rebuild,  CMake configure preset `ninja-x64-local` and build preset `debug-local` (Debug). Main application: `build.ps1 -SkipConfigure -ConfigurePreset ninja-x64-local -Target redclaw_desktop -NoPublish`. No application was launched or package published.
- CTest: 8/8 suites passed in 14.58 s: `redclaw_render_playback_flow_control_tests`, `redclaw_render_d3d11_shared_surface_bridge_tests`, `redclaw_video_codec_negotiation_tests`, `redclaw_runtime_options_tests`, `redclaw_helper_direct_frame_shared_snapshot_tests`, `redclaw_protocol_stream_control_protocol_tests`, `redclaw_stream_adaptation_regression_tests`, `redclaw_session_desktop_source_activity_tests`. Command: `ctest --test-dir build/ninja-x64 -C Debug --output-on-failure -R '^(redclaw_render_playback_flow_control_tests|redclaw_render_d3d11_shared_surface_bridge_tests|redclaw_video_codec_negotiation_tests|redclaw_runtime_options_tests|redclaw_helper_direct_frame_shared_snapshot_tests|redclaw_protocol_stream_control_protocol_tests|redclaw_stream_adaptation_regression_tests|redclaw_session_desktop_source_activity_tests)$'`. Generated evidence: `build/pause-decode-ctest-final.log`.
- UI receiver/window checks: `redclaw_ui_d3d11_integration_tests.exe --gtest_filter=DirectFrameReceiver.*:PlaybackVisibility.*`, 2/2 passed, including encoded-payload discard without snapshot/decode, reader acknowledgement, zero-frame resume, rapid toggles and fresh identity on receiver restart. Final incremental Debug build and these UI cases passed after the last UI changes. Evidence: `build/pause-decode-desktop-final.log` and `build/pause-decode-ui-final.log`.
- Docs-guard equivalent: 7 security Python tests, credential scan, required documents, 11 playbooks, all module headings, README entry contract and `git diff --check` passed. Cloud `build-and-test` and `docs-guard` were not submitted because this run is local-only and must not push. Full CTest and full UI integration sweep were not run; checks were focused on the touched path.
- Device and mixed-binary checks remain unchecked in the [PR #10 checklist](docs/testing/hevc-bitrate-measurement.md#pr-10-playback-pause-acceptance-checklist). No CPU/GPU/FPS improvement is claimed without a matched on-device run. Actual display-off/full-occlusion recovery, adapter/driver behavior, old-Host temporary FPS reduction and rolling upgrade/reconnect remain device-evaluation gaps.

### Native-window recreation regression follow-up

- Reproduced `D3D11Playback.NativeWindowRecreationRebindsPresentationWithoutReplacingDecoder` at `5d842cb`: access violation on iteration 0. CDB also caught Qt's `Cannot parent a QObject to itself` assertion during the subsequent native `create()`. The power-registration path called `winId()` from `WinIdChange` after Qt had cleared the HWND, recreating a native child inside the parent's teardown.
- Power registration now uses only `internalWinId()` and waits for a real replacement HWND. Missing native handles keep decode/presentation paused through the same visibility gate, including the 250 ms timer and occlusion-probe path. The existing failing test is unchanged; a separate zero-frame test holds the destroy/create gap across timer ticks and checks that no window/output is recreated before explicit creation and playback resumes afterward.
- VS2022 x64 Debug product build passed through `build.ps1 -Configuration Debug -NoPublish -SkipConfigure -ConfigurePreset ninja-x64-local -Target redclaw_desktop`; UI target rebuilt with `cmake --build --preset debug-local --target redclaw_ui_d3d11_integration_tests`. The final crash test plus both `PlaybackVisibility.*` tests passed 10 repetitions (30 cases). Full `ctest --test-dir build/ninja-x64 -C Debug --output-on-failure -R '^redclaw_ui_d3d11_integration_tests$'` passed all 12 cases, including unchanged decoder-device, output-HWND and present-result assertions. Generated evidence: `build/pause-decode-hwnd-{desktop-build,ui-build,final-repeat,ctest}.log` and `build/pause-decode-hwnd-before-cdb.log`.
- No pause-decode/feedback/HEVC policy changed; 4.4 remains hook-only. Actual display power/full occlusion, mixed-binary evaluation and performance measurements remain unrun. Work remained local to this worktree; no publish or live application/session changes.

### PR #10 review fixes at `4aa8df8`

D11 was recorded in `cdc01b3`; the previous review head `2748350` adjusted the visibility fixture. `cd81b23` was the earlier HWND recreation fix.

- A Controller keyframe request reopens a settled Host desktop as `kStaticPending` and queues a reference IDR from the retained capture. The GUI coalescer preserves a pending resume reason across dependency requests, and runtime recovery starts on a bounded deadline if the GUI notification is missing.
- D3D11 checks DWM cloaking and full window coverage every 250 ms, including with zero incoming frames. HEVC advertisement requires an explicit, enumerated presenter-adapter LUID with a successful probe and decoder pin. Qt/OpenGL, LUID 0, unknown adapters, and pin failures advertise H.264 only.
- The encoder stamps the revision used for codec selection and rechecks the selection after open. Running automatic HEVC falls back when the peer withdraws hardware decode. Confirmed network congestion can drain below the pacing floor after a local-pressure latch. The Windows frame-pipe sample is platform-guarded. The offline bitrate comparator checks the workload seed, scenarios and repetition count. The 70% HEVC scale still respects the 400 kbps interactive floor.
- VS2022 x64 Debug application build passed through `build.ps1 -SkipConfigure -ConfigurePreset ninja-x64-local -Target redclaw_desktop -NoPublish`. All 11 focused CTest suites passed, including all 13 D3D11 UI cases. Five repetitions of five D3D11 visibility, HWND recreation and receiver cases passed, including partial coverage, full coverage and zero-frame resume. The four offline comparator tests, credential check and diff check passed. Local evidence is in `build/reports/pr10-review-*.log`.
- D11 records that merges into `main` are gated on local builds and unit tests on both machines, a clean Codex re-review, on-device acceptance when needed, and GitHub `docs-guard`. GitHub `build-and-test` is advisory.
- Device and mixed-binary sessions, physical display-power and hardware IDR behavior, matched bitrate/quality measurement, the non-Windows build and the full CTest set remain unverified for this follow-up.

### PR #10 second-round re-review fixes (M02-T03)

- Local branch `codex/pause-decode-while-hidden`, on top of `4aa8df8`, D11 and the request-changes re-review of `2748350`. A resumed Controller redraws its last successfully presented picture while awaiting a fresh IDR; unpresented AUs still drain at the pause/resume boundary. A settled old Host can leave that picture visible at the same viewport until it supplies a new reference.
- A negotiated codec change wakes a static Host at the applicable deadline and encodes a retained reference IDR. The GUI authorizes hardware HEVC only on the enumerated monitor owner after probe and pin. D3D11 coverage polls DWM cloaking and subtracts bounding rectangles in Z order; a layered cover counts when alpha is 255 with no color key. Holes in shaped windows can overcount coverage, and non-opaque/per-pixel-alpha/color-keyed layers can undercount it. This is an accepted approximation.
- Geometry requests preserve pending recovery. Resume requests, submitted references and displayable ACKs stay bound to their playback identity. Fully assembled frames dropped locally are counted as reassembled and excluded from wire loss and decode-capacity samples.
- HEVC recovery requires an Annex B IDR (types 19/20). AMF HEVC is excluded from forced-refresh use; NVENC, QSV and diagnostic x265 require the forced-IDR setup, and missing forced IDR output triggers H.264 fallback. Queued audio messages carry the codec report current at send. The comparator requires every planned scene/repetition in each analysis; a subset is allowed only when selected by both plans.
- VS2022 x64 Debug product build passed with `build.ps1 -SkipConfigure -ConfigurePreset ninja-x64-local -Target redclaw_desktop -NoPublish`. All 8 focused Debug CTest suites passed: stream adaptation, codec negotiation, stream-control protocol, playback flow control, D3D11 shared surfaces, desktop source activity, FFmpeg roundtrip, and all 13 isolated D3D11 UI cases. The native-window/visibility/receiver subset also passed separately (5 cases). Six offline bitrate comparator tests, the credential scan and `git diff --check` passed. Evidence: `build/reports/pr10-rereview-round2-{desktop,ctest,ui-focused}.log`.
- Rechecked the previous findings: the resume reason and missed-notification deadline, requested static refresh, monitor pin/failure gate, encoder-open revision recheck, 400 kbps floor, network escalation and non-Windows guard remain in place. The non-Windows build and full CTest sweep were not run.
- D11 remains open at the final head: both machines' local builds and unit gates, a fresh independent read-only re-review, mixed-version and hardware acceptance (including idle resume and refresh IDRs), and GitHub `docs-guard`. Bitrate/quality and performance remain unmeasured for this round. No push, publication or live RedClaw process changes.

### PR #10 final review fixes on `efb8950` (M02-T03)

- D3D11 rebuilds the replacement HWND's output before redrawing its retained CPU or GPU picture. Native-window recovery still requests an IDR. A missing native parent client rectangle during mapping no longer registers as full coverage. The poll still clips to valid parent rectangles and counts opaque click-through layered covers.
- Receiver feedback is checked against current playback session, revision and pause state at every reliable-channel send attempt. The reassembler validates all fragments during IDR wait without copying dependent payloads. Complete local discards count as reassembled and genuine incomplete AUs still count as network loss. An HEVC reopen failure or stopped context schedules an H.264 restart through the existing reference-refresh path.
- The four reported D3D11 tests now raise topmost fixtures and wait up to two seconds for the mapped, exposed client and the active playback callback. The desktop baseline passed at `efb8950`. The laptop-specific failure and the fix on that machine remain unverified.
- VS2022 x64 Debug built with `build.ps1 -SkipConfigure -ConfigurePreset ninja-x64-local -Target redclaw_desktop -NoPublish`. Focused `ctest --test-dir build/ninja-x64 -C Debug --output-on-failure` passed all 13 selected suites, including all 13 D3D11 UI cases. The four reported cases passed five consecutive repetitions (20/20). Evidence is in `build/reports/pr10-final-desktop.log`, `pr10-final-ctest.log` and `pr10-final-d3d11-repeat.log`.
- The encoder tests cover restart classification, including a failed reopen and a stopped encoder. Hardware reopen fault injection, live feedback retry, mixed-version/device acceptance and the laptop rerun remain open. The existing D10 and D11 decisions apply.

### PR #10 keyframe send follow-up (M02-T03, 2026-11-13)

- Finding 1 fixed in this commit: first sends and retries consume keyframe requests whose playback session or revision has changed or whose playback is paused. Consumed requests are neither sent nor requeued.
- Finding 2 accepted in [D10](DECISIONS.md#d10--hevc-is-preferred-when-both-peers-have-hardware-hevc-h264-stays-the-fallback): the coverage poll does not detect non-layered `WS_EX_TRANSPARENT` windows that paint opaque rectangles as full covers. Coverage code is unchanged.
- The VS2022 x64 Debug application build passed without publishing. The runtime options, stream-control protocol and desktop source activity CTest suites passed, including the new keyframe send identity regression. Live sessions were not exercised.

### PR #10 capture-retry send exemption (M02-T03, 2026-11-13)

- Manual `user_capture_retry` keyframe requests are exempt from the playback session/revision/pause send fence. The GUI no longer stamps or drops those requests when playback is unmapped, paused, or on a different session. Ordinary resume/recovery keyframe requests and receiver feedback stay fenced.

## Mainline

- PR #4 (`eca4b31`) is merged: root `SPEC.md`, `PROGRESS.md`, `DECISIONS.md`, and the root `AGENTS.md` onboarding gate.

- `40b80cc` is the merge of PR #1, "Remove the Connection overlay log and clean logs on startup" (https://github.com/70565912/RedClawDesktop/pull/1). Author Ryan Chen. Merged 2026-11-05 14:46 UTC (2026-11-05 22:46 Asia/Shanghai).
- The floating Connection panel no longer shows the scrolling runtime log. Progress, pairing, handshake, live link, path and picture status, and reconnect/disconnect stay. The connection page keeps its own log. Diagnostics stay in capacity-limited files. The same behavior is described in [README.md](README.md) and the 2026-11-05 section of [PROJECT_STATE.md](docs/runtime/PROJECT_STATE.md).
- PR #1 checks that completed before merge: `docs-guard` success, `hint` success, `build-and-test` success (Actions run `42042846320`). The PR body says the authoring environment had no VS2022 toolchain. No new dual-machine visual acceptance for this merge is written in the tree.
- The last published package is the v0.1.11 Developer Preview ([release notes](docs/releases/v0.1.11.md)). Version metadata and [v0.1.12 release notes](docs/releases/v0.1.12.md) are prepared. `main` since that tag includes the dark workspace, the connection overlay, overlay-log removal (PR #1, `40b80cc`), the root status files (PR #4, `eca4b31`), and the clipboard paste repair (`c8ec2a2`).
- The Android spike is not on `main`. Searches for `android-controller-spike` and `M11-S0` found nothing. Kanban `M11-T01` remains `planned`.
- On `main`, the Host selects H.264 in `src/main.cpp` at the startup/resize profile (`preferred_codec`), the bridge request (`codec`), and the adaptive profile (`preferred_codec`), all `EncoderCodec::kH264`. `EncoderCodec::kHevc` mapping exists. A search of `src/` found no `video_decode` capability field. That matches the "Host sends H.264" and "no decoder capability" claims in PR #3. The rest of that PR's risk table was not re-measured here.

## Open pull requests

Do not merge either pull request as part of this snapshot.

### PR #2, draft — Android Controller spike

https://github.com/70565912/RedClawDesktop/pull/2

- Draft, open, not merged. Head `d3b2a046303c6ae2ff2329e96b622a581f81d619` on `cursor/android-controller-spike-s0-1-9958`. Base recorded as `main` @ `40b80cc`. GitHub `mergeable_state` at survey time: `dirty`.
- S0.1, as described by the PR: portable `redclaw_controller_core` plus an arm64 JNI / MediaCodec shell. Not a Host and not the Flutter companion. Terminal, Agent, and transfer are out of that spike's scope.
- Operator comment on 2026-11-06 00:56 UTC (08:56 Asia/Shanghai), GitHub login `70565912`: pause the spike and return to `main`. S0.1 scaffold and the debug APK (`android-spike-s0.1` prerelease) are OK. S0.2 on a cellular path reached DHT `answer_published` / Host `answer_applied` and `ice_connecting`, then ICE failed (`phase=failed`, no media). No TURN, by product choice (prefer free P2P). Resume later on the same LAN or another free direct path. Do not merge until S0.2–S0.5 pass. The branch is kept and the PR was converted to draft.
- The only check run returned for this head was `windows-portable-package`, conclusion failure (Actions run `42061689311`, about 11 seconds). The cause was not investigated for this snapshot.
- Codex review on the PR did not complete (usage limit).

### PR #3, open — HEVC assessment, documentation only

https://github.com/70565912/RedClawDesktop/pull/3

- Open, not a draft, not merged. Head `4f437ae19ab6f56bfa8127af8aefdfc9b1767f90` on `cursor/hevc-migration-assessment-6a79`. Base `main` @ `40b80cc`. GitHub `mergeable_state` at survey time: `unstable`.
- The PR adds `docs/architecture/hevc-migration-assessment.md` and index, kanban, and devlog links. It states that it does not change encoder, decoder, protocol, or runtime behavior. That file is not on `main`.
- Recommendation in the PR body: do not switch the default to HEVC. Optional HEVC, in phases, with H.264 remaining the default and the fallback. A Controller decoder-capability report comes before the Host may choose HEVC. Forced IDR and tests come before any behavior change. Android MediaCodec on the sealed spike is left untouched.
- No HEVC encode or decode run and no bitrate or latency measurement were made for this snapshot. Numbers in the PR are that PR's hypotheses.
- At survey time, `docs-guard` and `hint` had succeeded and `build-and-test` was still in progress (Actions run `42141441966`). Codex review did not complete (usage limit).

## Optional debug Host

PR #2 names Host machine code `RC7TST01` as the LAN target for S0.2. That string is not in the `main` tree. The 2026-11-06 seal comment does not repeat it. It describes the cellular ICE failure.

Treat `RC7TST01` as optional context for a future LAN resume, not as a current `main` task. This survey did not contact any Host. Whether that machine is still in Debug, still publishing, or still the resume target is for Ryan to confirm. Do not add its password or a local debug config to git.

## Next steps

Items 1–5 remain open. Item 6 is the publish step after this preparation is on `main`.

1. Leave PR #2 draft and sealed. Do not merge, rebase, or continue Android connect, decode, or input work until Ryan asks for a same-LAN or other free-P2P resume.
2. HEVC (D10): review the HEVC pull request and its Windows CI. Before merging, run its on-device checklist on Ryan's desktop and laptop, at least: hardware HEVC opens, forced IDR on recovery, mixed old/new versions in both directions, and the `h264` rollback. After that, run [HEVC bitrate measurement](docs/testing/hevc-bitrate-measurement.md) and record the result here. PR #3 (assessment document) can be merged or closed at Ryan's choice.
3. Keep the Windows product on direct-first P2P. TURN stays optional and user-provided. Do not add a product-owned relay to paper over the cellular ICE failure.
4. The next phase named in [README.md](README.md) — wider cross-site and TURN coverage, unattended install and upgrade, code signing, and lower GUI / Agent scheduling cost — stays backlog. It is not the next edit unless Ryan assigns it.
5. Ryan confirms the open questions in [SPEC.md](SPEC.md): Flutter companion versus the sealed spike, and whether `RC7TST01` is still relevant.
6. From a clean Windows checkout of `main`, run `scripts/release/publish-github-release.ps1 -Version 0.1.12`. That step creates the tag and the GitHub Pre-release. This preparation does not run it.
