# P0 Full Machine-Code Desktop Stream Validation

This is the required product validation path. Direct IP/TCP and preview-only smoke tests are diagnostic-only and do not satisfy P0.

## Required User Workflow

1. Start RedClawDesktop on both machines.
2. One side generates and shares its machine code.
3. The other side enters only that peer machine code.
4. The app uses DHT-backed online rendezvous signaling and ICE/STUN/IPv6/port mapping/UDP hole punching to connect across LAN/NAT boundaries when a serverless path exists.
5. Host captures the desktop.
6. Host encodes captured frames with the real video encoder path.
7. The runtime transports encoded video frames.
8. Controller decodes the frames.
9. Controller UI plays the decoded desktop image.

The user workflow must not require entering a LAN IP address.

## Required Runtime Path

```text
machine code -> DHT-backed rendezvous signaling -> ICE/STUN/IPv6/port mapping/UDP hole punching -> capture -> H.264/HEVC encode -> transport -> decode -> UI playback
```

## UI Operator Setup

On both machines, configure the desktop UI before starting the run:

- The customer shell now defaults the visible workflow to machine-code DHT pairing. `Generate Code` and `Connect` are the only customer-facing actions on the pairing screen.
- Advanced transport, DHT bootstrap, ICE, helper, and profile fields are intentionally hidden from the visible shell. Use runtime profiles or internal diagnostic workflows only when evidence collection requires those overrides.
- Do not require a RedClaw-owned public `Rendezvous URL` for normal DHT mode. TURN/relay remains optional and only user-provided when the serverless path cannot connect.

Machine-code actions:

- Host: click `Generate Code`. The UI generates the local code, switches to host mode, and starts waiting immediately.
- Controller: paste the Host machine code, then click `Connect`. The UI validates the peer code, switches to controller mode, and starts connecting immediately.

Diagnostics to watch in the UI during the run:

- `Connect` tab: hero guidance, local machine code, peer code entry, `Generate Code`, `Connect`, and a customer-facing status banner.
- `Live View` tab: session status cards, floating connection banner, and `Remote Desktop` playback.
- Runtime diagnostics still exist internally, but they are no longer shown on the customer-facing shell by default.

## Hard Pass Criteria

Host:
- Uses `signal_transport=dht`.
- Does not require `target_host`.
- Logs DHT publication and NAT diagnostics.
- ICE candidate logs include host/srflx/relay classification.
- `captured > 0`.
- `encoded > 0`.
- `encode_failures = 0`.
- `transmitted > 0`.

Controller:
- Uses `signal_transport=dht`.
- Connects by peer machine code only.
- UI `Remote Desktop Playback` shows decoded Host desktop video.
- `received > 0`.
- `decoded > 0`.
- `rendered > 0`.
- `decode_failures = 0`.

Network:
- Must be validated across two different LANs.
- DHT rendezvous must be used for offer/answer/candidate exchange without a RedClaw-owned public server.
- IPv6, port-mapping, STUN, and hole-punch diagnostics must be archived.
- When direct serverless traversal fails, the run must classify the blocker and state whether a user-provided relay/helper is required.

## Serverless helper notes

- A local helper may run on the Host, Controller, another LAN computer, or a supported NAS runtime.
- It can maintain DHT state, create allowed router mappings, hold local encrypted records, and provide LAN diagnostics.
- It acts as an Internet relay only when it is reachable through public IPv6, an explicit router mapping, a supported tunnel, or another operator-controlled path.

## Current Implementation Status

Implemented or in progress:
- UI machine-code workflow now defaults both `Generate Code` and `Connect` to `signal_transport=dht`.
- Customer-facing UI now hides transport/profile/diagnostic controls and keeps only the machine-code pairing flow plus simplified live playback status visible.
- Runtime diagnostics now stay behind internal widgets/log capture instead of a customer-visible diagnostics tab.
- Runtime DHT adapter supports encrypted in-memory and file-backed local stores for deterministic tests and diagnostics.
- Runtime libtorrent Mainline DHT publishing/fetching is covered by local offer/answer, candidate exchange, ICE connection, and DataChannel tests.
- Runtime stream counters/failure classification.
- Host capture path with DDA and GDI fallback.
- Host libavcodec encoder execution path.
- Controller libavcodec decoder path with hardware-preferred FFmpeg fallback is implemented; remaining gate is real dual-machine evidence.

Still required before this runbook can pass:
- Cross-LAN DHT/ICE/NAT evidence using two different LANs.
- NAT traversal evidence for UPnP/PCP/NAT-PMP, STUN, IPv6, and synchronized hole punching, including a clear relay-required classification if no serverless path connects.
- Optional user-provided relay/helper evidence only when the serverless path cannot connect.
- End-to-end UI run on two different LANs.

## DHT Validation Helpers

Use these helpers when one side is on a remote network and both sides need repeatable CLI logs before or alongside the full UI pass.

Bootstrap readiness preflight, to catch fake-IP DNS or unusable bootstrap resolution before starting Host/Controller helpers:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/test-dht-bootstrap-readiness.ps1
```

This writes `build/reports/dht-bootstrap-readiness-<timestamp>/summary.json` with local DNS results, optional DoH comparison, known runtime numeric fallbacks, and per-node status. Exit code `0` means `ready`, exit code `1` means warning or partial usability such as fake-IP DNS with cached runtime fallbacks, and exit code `2` means no usable bootstrap node was found. If this preflight reports `warning_suspicious_or_partial_bootstrap`, keep the default bootstrap hostnames in the helper/UI input; the runtime now normalizes them to cached numeric endpoints before libtorrent bootstrap, so FlClash-style `28.0.0.x` fake DNS no longer blocks DHT join on its own.

The Host and Controller helper scripts also run this preflight automatically and write `bootstrap-readiness.json` inside their report directories. A warning-level preflight does not stop the runtime; it is carried into the helper summary as `bootstrap_readiness_warning`. A `blocked_no_usable_bootstrap` preflight stops before launching the runtime and the summarizer classifies the report as `bootstrap_blocked`.

Local preflight, to verify the published runtime, DHT helpers, and summarizer on one machine before a remote run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-local-dht-helper-pair-validation.ps1 -RunSeconds 100 -WaitSeconds 140 -StopAfterConnectedSeconds 20
```

When `-SessionCode` is omitted, the local preflight generates a temporary 8-character code with the OS cryptographic random number generator and still only prints/stores the redacted form.

Remote-run command preparation, to verify local prerequisites and print the Host/Controller workflow commands without launching runtime or network probes:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/prepare-dht-remote-validation.ps1 -Role both
```

This writes `build/reports/dht-remote-validation-prep-<run-id>.json`, checks the published runtime and DHT helper scripts, and prints commands with `<HOST_CODE>` placeholders. If `-SessionCode <HOST_CODE>` is provided, the script validates the 8-character shape but still redacts it in output and keeps `<HOST_CODE>` in generated commands so full connection codes are not stored in prep reports.

The prep report also checks `published_runtime_fresh` by comparing `release\<Configuration>\redclaw_desktop.exe` against key DHT/runtime source files. If a source file is newer than the published runtime, rebuild and publish before a remote run; otherwise the evidence may describe stale code.

Host side, to publish a waiting machine code and keep the process alive for a remote Controller:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-dht-host-validation.ps1 -SessionCode <HOST_CODE> -RunSeconds 240 -WaitSeconds 300 -StopAfterConnectedSeconds 20
```

Controller side, to connect to a waiting remote Host code:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-dht-controller-validation.ps1 -SessionCode <HOST_CODE> -RunSeconds 240 -WaitSeconds 300 -StopAfterConnectedSeconds 20
```

One-command single-role evidence workflow, recommended when one operator controls only one side of a cross-LAN run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-dht-role-validation-workflow.ps1 -Role controller -SessionCode <HOST_CODE> -RunSeconds 240 -WaitSeconds 300 -StopAfterConnectedSeconds 20 -CreateZip -IncludeExistingReportRegression
```

Use `-Role host` with the same command shape when this machine is the waiting Host. The workflow runs the role helper, writes a single-role summary, then archives the report with `-IncludeToolchainSelfTest` enabled by default. This is the shortest CLI evidence path for remote DHT/ICE diagnosis; it still does not replace the full UI playback proof.

The workflow also writes `build/reports/dht-<role>-workflow-result-<run-id>.json` and `build/reports/dht-<role>-workflow-result-summary-<run-id>.json`. The run id is a timestamp plus a short GUID suffix so quick retries do not overwrite evidence. The result JSON is the machine-readable handoff entry for remote diagnosis: it records `status`, redacted session code, result-summary path, report directory, summary path, archive/zip path, zip SHA256, per-stage exit codes, and an `error_message` when setup, helper, summary, or archive fails. Share the result JSON, result-summary JSON, and zip SHA256 before interpreting console logs.

The single-role workflow runs the result summarizer automatically. To re-summarize one or more workflow result files directly:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/summarize-dht-workflow-result.ps1 -WorkflowResult <WORKFLOW_RESULT_JSON> -OutputPath build/reports/dht-workflow-summary.json
```

This reads the workflow result, verifies any referenced evidence zip hash when the zip is available, loads the workflow summary or archived `dht-summary.json`, and emits a stable `overall_status` such as `success_connected`, `dry_run`, `runtime_missing`, `bootstrap_blocked`, `dht_publish_missing`, `dht_remote_record_missing`, `dht_candidates_missing`, or `ice_failed_after_dht_exchange`. Use comma-separated `-WorkflowResult` values when both sides provide result JSON files. A non-success classification exits non-zero so the command can be used in handoff checks. New workflow summaries also preserve the raw final `Runtime candidate stats`, `Runtime DHT stats`, `Runtime DHT backend diagnostics`, and `Runtime NAT diagnostics` lines under each report classification; use those fields for first-pass remote triage before digging into stdout logs.

The workflow and report summaries also include `triage_hint`. Treat it as the operator's next diagnostic direction, not as the final verdict. For example, `ice_failed_after_dht_exchange` should shift the next check to ICE/NAT candidate pairs, IPv6/port mapping, and possible TURN/helper needs because DHT rendezvous already succeeded; `dht_remote_record_missing` should instead check same-code overlap and whether the peer actually reached `publish_success`.

Pitfall: `status=helper_failed` in `dht-<role>-workflow-result-*.json` is only the outer workflow/helper exit status. Do not treat it as the network-stage classification when the embedded workflow summary or archived `dht-summary.json` is present. A runtime can publish/fetch through public DHT and then exit non-zero because ICE failed; that case must remain `ice_failed_after_dht_exchange` with the raw DHT/backend/NAT lines preserved, not collapse back to generic `helper_failed`. The regression case is covered by `workflow_result_ice_failed_summary` in `scripts/service/test-dht-validation-toolchain.ps1`.

Candidate timing note: the DHT runtime now publishes candidate material in small, direct steps before attempting the larger full-candidate snapshot. The expected phases are `description-only`, `priority-candidates`, one or more `direct-candidate` records, then `full` when the complete snapshot is available. Candidate summaries include `local_dht_direct_published`, `local_dht_full_published`, and `remote_dht_latest_candidates`. If ICE fails after only `remote_dht_exchanged=1`, inspect these fields before blaming STUN/TURN: the peer may have seen only the first server-reflexive candidate and not yet the later direct candidate/full snapshot.

Pitfall: do not triage a remote workflow from pasted console output alone. `summarize-dht-workflow-result.ps1` now prints `remote_dht_exchanged`, `local_direct`, `local_full`, and `remote_latest` in its human-readable `report role=...` line, and writes the full per-report candidate progress fields into the summary JSON (`local_dht_direct_published`, `local_dht_full_published`, `remote_dht_latest_candidates`). For remote handoff, still require the result-summary JSON or evidence zip before making a final call, because JSON remains the source of truth for DHT rendezvous, candidate timing, and ICE/NAT classification.

The workflow-result summary also writes `role_coverage`, `p0_ds07_gate_status`, and `next_action`. A single-side success reports `role_coverage.workflow_scope=single_role_cli` and `p0_ds07_gate_status=single_role_cli_connected_peer_result_required`; collect the peer result before treating the CLI evidence as a pair. A Host+Controller CLI pair success reports `p0_ds07_gate_status=cli_pair_connected_ui_playback_required`; move to Controller UI playback capture, because CLI workflow success still is not the P0-DS-07 acceptance artifact.

Notes:

- Each script runs the published runtime from `release\<Configuration>\redclaw_desktop.exe`; when the build tree is already configured and only the desktop runtime is stale, build and publish with `.\build.ps1 -Configuration Debug -SkipConfigure -Target redclaw_desktop -Parallel 1`. Use full `.\build.ps1` only when configure/dependency state must be refreshed.
- Both scripts force `signal_transport=dht`, the same default public DHT bootstrap set as the runtime (`router.bittorrent.com:6881`, `dht.transmissionbt.com:6881`, `dht.libtorrent.org:25401`, `router.utorrent.com:6881`), and STUN `stun:stun.l.google.com:19302`. This deliberate pin keeps scripted evidence comparable; the interactive GUI instead defaults to the single-select `Automatic` policy, which prefers responsive Douyu CDN, otherwise selects the fastest responding Google/Cloudflare alternative, and falls back to Douyu when every bounded probe fails. At runtime DHT hostnames are normalized to cached numeric bootstrap endpoints before libtorrent session setup.
- Both scripts rely on the runtime default private `signal_dir/dht-mailbox` store, so `helper_status` reflects local private state rather than a shared folder.
- Host evidence is written under `build/reports/dht-host-validation-<timestamp>/`; Controller evidence is written under `build/reports/dht-controller-validation-<timestamp>/`.
- Both scripts redact the session code in console output and summary JSON; do not paste unredacted codes into archived notes unless the code has expired.
- Successful CLI helper runs are not the full P0 pass. Full acceptance still requires the visible Controller UI playback evidence listed below.

After the run, summarize one or both report directories:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/summarize-dht-validation-report.ps1 -ReportDirectory <HOST_REPORT_DIR>,<CONTROLLER_REPORT_DIR>
```

The summary classifies the observed failure stage as `bootstrap_blocked`, `dht_not_reachable`, `dht_publish_missing`, `dht_remote_record_missing`, `dht_candidates_missing`, `ice_failed_after_dht_exchange`, `ice_pending_after_dht_exchange`, or `connected`. Use `-Json` or `-OutputPath <path>` when the result should be attached to an evidence bundle.

Warnings such as `connected_before_dht_publish_success_alert` or `connected_without_remote_dht_candidate_counter` mean the data channel opened before the final DHT alert/candidate heartbeat caught up. Treat those as timing notes for the log bundle, not as a rendezvous failure when `overall_status` is `success_connected`. In JSON output, `warnings` is always an array; a clean report writes `[]`.

Archive the helper reports and combined summary before sharing or handing off a run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/archive-dht-validation-evidence.ps1 -ReportDirectory <HOST_REPORT_DIR>,<CONTROLLER_REPORT_DIR> -IncludeToolchainSelfTest -IncludeExistingReportRegression -CreateZip
```

The archive command copies the report directories into `build/reports/dht-validation-evidence-<timestamp>/`, writes `dht-summary.json`, writes `archive-manifest.json` with SHA256 hashes for the bundled files, and with `-CreateZip` also writes a sibling `.zip` plus its SHA256. `-IncludeToolchainSelfTest` embeds `toolchain-selftest-summary.json` in the bundle so the recipient can see that the local report classifier passed its deterministic checks when the archive was created.

Before changing these helper scripts or after resolving a DHT report-classification bug, run the deterministic toolchain self-test:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/test-dht-validation-toolchain.ps1 -IncludeExistingReportRegression
```

The default self-test is offline and synthetic: it checks `dry_run`, `bootstrap_blocked`, connected-with-bootstrap-warning, explicit DHT/ICE failure classifications, workflow-result gate states, and raw final diagnostic-line preservation. `-IncludeExistingReportRegression` also re-summarizes the latest checked-in local evidence directory when present. This test validates the helper/summarizer/archive evidence path only; it does not replace live DHT, cross-LAN, or UI playback validation.

After the Host+Controller workflow summary reaches `p0_ds07_gate_status=cli_pair_connected_ui_playback_required`, run the P0 evidence gate with the UI screenshot/video and final runtime logs:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/test-p0-ds07-evidence-gate.ps1 `
  -WorkflowSummary <WORKFLOW_SUMMARY_JSON> `
  -UiEvidence <CONTROLLER_UI_SCREENSHOT_OR_VIDEO> `
  -HostRuntimeLog <HOST_RUNTIME_LOG> `
  -ControllerRuntimeLog <CONTROLLER_RUNTIME_LOG> `
  -CrossLanConfirmed `
  -OutputPath build/reports/p0-ds07-evidence-gate.json
```

The gate does not start the runtime or prove network topology by itself. It fails unless the workflow summary is a connected Host+Controller pair, `-CrossLanConfirmed` is present, at least one non-empty UI evidence file exists, the Host log has final `captured>0 encoded>0 transmitted>0 encode_failures=0`, the Controller runtime log has final `received>0 encoded_frames_reassembled>0 direct_pipe_written>0`, and the Controller GUI log has final `gui_decode_success_total>0 presented_total>0 decode_failures=0 present_failures=0`. Runtime delivery into the GUI is not counted as decode/render. Use the output JSON as the final checklist before handoff review.

`-HostRuntimeLog` and `-ControllerRuntimeLog` are optional only when the workflow summary already points at per-role report directories whose `summary.json` files reference stdout logs containing final stream stats. If the UI run writes separate runtime logs, pass those files explicitly so the gate checks the UI playback run rather than the earlier CLI helper run.

## Cross-Site Handoff (异地联调)

**Canonical checklist:** Use `docs/testing/cross-lan-dual-machine-integration-playbook.md` as the step-by-step playbook for the next cross-LAN round. It mandates GUI-only integration with fixed session code `RC7TST01`, standby startup through `start-cross-lan-debug-supervisor.ps1`, local current-user control through `invoke-cross-lan-debug-control.ps1`, dual-side evidence export, and P0 Live View evidence. This section remains the detailed reference for CLI helper scripts and P0 gate commands.

Use this section when one machine acts as the waiting Host on a remote LAN and the other machine acts as the Controller. The goal is: sync code, build once per machine, start Host GUI first with fixed code `RC7TST01`, then connect from Controller GUI.

### Preferred controlled GUI launch (fixed code)

Host (remote, start first and keep the window open):

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/start-cross-lan-debug-supervisor.ps1 -Role host -RunId <RUN_ID>
```

Controller (local, start in standby before Host runtime):

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/start-cross-lan-debug-supervisor.ps1 -Role controller -RunId <RUN_ID>
```

The fixed code is defined in `scripts/service/cross-lan-integration-config.ps1` (default `RC7TST01`). Start Host/Controller runtime only after both supervisors report `phase=idle`, following the timed gates in the canonical playbook. `run-cross-lan-gui-integration.ps1` remains the manual auto-start wrapper.

### 1. Sync code on both machines

```powershell
git clone https://github.com/70565912/RedClawDesktop.git
cd RedClawDesktop
git fetch origin
git checkout <FEATURE_BRANCH>
git pull --ff-only origin <FEATURE_BRANCH>
git rev-parse HEAD
```

If the repo is already cloned, run only `git pull origin main` before each new test round.

### 2. One-time setup on each machine

1. Install Visual Studio 2022 with the MSVC C++ toolchain and CMake support.
2. Install vcpkg and set `VCPKG_ROOT` in the environment.
3. Copy `CMakeUserPresets.example.json` to `CMakeUserPresets.json` and set local `VCPKG_ROOT` / toolchain paths.
4. From the repo root, configure and publish the runtime:

```powershell
.\build.ps1
```

For rebuilds after pulling new commits:

```powershell
.\build.ps1 -SkipConfigure
```

Use `.\build.ps1 -Configuration Release` on the remote Host if you want the Release runtime. The validation helpers default to Debug.

If `ffmpeg:x64-windows` fails on a Chinese-locale MSVC machine, see `docs/setup/vcpkg-ffmpeg-locale-troubleshooting.md`.

### 3. Preflight on each machine before the cross-LAN run

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/test-dht-bootstrap-readiness.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/prepare-dht-remote-validation.ps1 -Role both -Json
```

`prepare-dht-remote-validation.ps1` checks that `release\Debug\redclaw_desktop.exe` exists and is not older than the current DHT/runtime sources. If `published_runtime_fresh=false`, rebuild before starting the remote Host.

### 4. Start the remote Host (server side)

On the remote development machine:

**UI path (preferred for final evidence):**

1. Run `release\Debug\redclaw_desktop.exe`.
2. Click `Generate Code`.
3. Share the 8-character machine code with the Controller operator through a secure channel.
4. Keep the app waiting until the Controller connects.

**CLI evidence path (recommended for first cross-LAN diagnosis):**

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-dht-role-validation-workflow.ps1 `
  -Role host `
  -SessionCode <HOST_CODE> `
  -RunSeconds 240 `
  -WaitSeconds 300 `
  -StopAfterConnectedSeconds 20 `
  -CreateZip `
  -IncludeExistingReportRegression
```

Replace `<HOST_CODE>` with `RC7TST01` (fixed integration code) unless you are running a one-off diagnostic with a different code. For the formal controlled run, start and query the Host through the supervisor/control scripts in the canonical playbook and keep the GUI process running.

### 5. Connect from the Controller machine

**UI path:**

1. Run `release\Debug\redclaw_desktop.exe`.
2. Enter the remote Host machine code.
3. Click `Connect`.
4. Open `Live View` and confirm decoded desktop playback.

**CLI evidence path:**

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-dht-role-validation-workflow.ps1 `
  -Role controller `
  -SessionCode <HOST_CODE> `
  -RunSeconds 240 `
  -WaitSeconds 300 `
  -StopAfterConnectedSeconds 20 `
  -CreateZip `
  -IncludeExistingReportRegression
```

### 6. Classify and archive the run

After both sides finish, share these artifacts before deeper log review:

- `build/reports/dht-host-workflow-result-<run-id>.json`
- `build/reports/dht-controller-workflow-result-<run-id>.json`
- matching `dht-*-workflow-evidence-*.zip` files and their SHA256 values

Re-summarize both workflow results together:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/summarize-dht-workflow-result.ps1 `
  -WorkflowResult <HOST_WORKFLOW_RESULT_JSON>,<CONTROLLER_WORKFLOW_RESULT_JSON>
```

Interpretation:

- `overall_status=success_connected` and `p0_ds07_gate_status=cli_pair_connected_ui_playback_required` means DHT/ICE connected at CLI level; continue to Controller UI playback capture.
- `overall_status=ice_failed_after_dht_exchange` means DHT rendezvous worked but NAT/ICE failed; inspect candidate stats, IPv6/port mapping, and possible TURN/helper need.
- `overall_status=bootstrap_blocked` means fix bootstrap/DNS before retrying.

### 7. Minimum handoff checklist

- Both machines pulled the same `main` commit.
- Both machines built with `.\build.ps1` and can run `release\Debug\redclaw_desktop.exe`.
- Bootstrap preflight is at least warning-level usable, not `blocked_no_usable_bootstrap`.
- Host generated the machine code and stayed online during the Controller attempt.
- Controller used only the machine code, not an IP address.
- Evidence bundle includes workflow result JSON, summary JSON, and UI playback proof when available.

## Evidence To Archive

- Controller UI screenshot/video showing decoded desktop playback.
- Host final `Runtime desktop stream stats`.
- Controller final `Runtime desktop stream stats`.
- ICE candidate stats showing direct or relay path.
- DHT logs showing encrypted snapshot publish/fetch counters and the backend used.
- DHT bootstrap/helper configuration and NAT traversal diagnostics used for the run.
- Relay/helper configuration only when one is used.
