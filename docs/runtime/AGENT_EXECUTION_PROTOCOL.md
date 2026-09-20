# Agent Execution Protocol (Product-first, local automation first)

Purpose: any agent can start productive work from a single entry (`README.md`) and drive the current highest-priority product milestone: dual-machine desktop image transfer and display.

## Required Input
- Optional goal or slice ID, e.g. `P0-DS-01`.
- Optional module ID, e.g. `M01`, only when the user intentionally wants module-scoped work.

If no explicit slice is provided, agent follows `README.md` links to `docs/runtime/PROJECT_STATE.md` and picks the first unfinished item in the active priority queue.

## Mandatory Reading
1. `README.md` — single mandatory entry for every handoff.

## On-Demand Reading (from README links)
1. `docs/runtime/PROJECT_STATE.md` — current stage, focus modules, blockers.
2. `docs/runtime/MODULE_KANBAN.md` — status ledger and deferred module work.
3. `AGENTS.md` — current execution guardrails and validation policy.

## Extended Context (read when needed, not mandatory on every start)
- `docs/modules/module-specs.md` — interface contracts when changing shared APIs.
- `docs/runtime/MODULE_ASSIGNMENT_CONTRACT.md` — assignment payload and completion rules.
- `docs/testing/test-matrix.md` — authoritative local/dual-endpoint/cross-LAN routing, automated entry and evidence rules.
- `docs/setup/coding-conventions.md` — naming, formatting, testing framework, security constants.
- `docs/logs/DEVLOG.md` (latest phase summary only) — recent decisions and risks.

## Start-Work Algorithm
1. Read `docs/runtime/PROJECT_STATE.md`.
2. Select the first unfinished `P0-DS-*` item from the active priority queue unless the user explicitly names another task.
3. Identify the smallest vertical slice that moves the desktop-stream path forward.
4. Modify all modules needed for that slice; do not split the work only to preserve old module boundaries.
5. Select affected automatic checks from the test matrix; build only changed targets and collect stream evidence only when the changed risk crosses that boundary.
6. Update `docs/runtime/MODULE_KANBAN.md` with the slice result or blocker.
7. Record a summary entry in `docs/logs/DEVLOG.md`.
8. If handing off, append handoff block per `docs/agent-handoff/AGENT_HANDOFF_PROTOCOL.md`.

## Resuming Work (Continuation)
When resuming after a pause or handoff:
1. Read `docs/runtime/PROJECT_STATE.md` for current resume point.
2. Read latest DEVLOG phase summary for recent context and open risks.
3. Check `docs/runtime/PROJECT_STATE.md` active priority queue and the top priority section in `docs/runtime/MODULE_KANBAN.md`.
4. Continue from the first unfinished desktop-stream slice.

## Scope Guardrails
- Cross-module changes are allowed when they directly support Host capture -> Controller display.
- Do not spend time on unrelated module cleanup, broad assertion migration, format-only churn, mobile companion work, clipboard/file transfer, or exhaustive matrices before P0 visual proof.
- If cross-module interface change is needed, record the interface-change note in `DEVLOG`.
- Never fake desktop-stream evidence. If a run cannot be executed, record the exact blocker.
- P0 desktop-stream performance work must not use default or automatic input-size reduction as a bottleneck workaround. Keep native capture/encode dimensions by default (`stream_video_max_width=0`); treat `--stream-video-max-width` only as an explicit manual weak-network or diagnostic cap. Prefer FPS/bitrate/backpressure control for adaptive behavior, and prioritize the DDA/D3D11 texture-to-hardware-encoder path for structural Host-side cost reduction.

## Validation Policy

- Follow the [test matrix](../testing/test-matrix.md): A local isolated/native, B local Host/Controller, C minimal physical cross-LAN. These prove different risks; they are not an ascending mandatory ladder for every case.
- Default to `scripts/service/run-local-validation.ps1 -Area <affected areas>` for focused automatic build/test/reporting. Combine areas once; shared tests run once. Register unattended cases only; manual cases must not become skipped/pending automatic results. Unexpected skips are test/environment errors, not a request for human completion.
- Build changed application targets through `build.ps1`; test-only target builds may use guarded CMake. Documentation-only changes need no compile; script changes need their own regression and a representative real invocation. Do not rebuild both Debug/Release, run full CTest or redeploy both endpoints for each iteration.
- Use existing local dual-GUI automation for cross-component behavior, supported-version directions, rolling upgrade and reconnect. Record negotiated common capabilities; never require equal commits/versions/hashes. Local success does not claim NAT/TURN coverage.
- C covers real rendezvous/NAT/TURN and environment-specific delivery only when unattended fixtures exist. Otherwise it is an optional developer evaluation, not an automatically requested remote session or release requirement. Protocol, UI, file edge cases and terminal feature matrices stay local.
- Prefer machine-readable outcomes. A clear target application response is sufficient application-consumption evidence; a second peer Agent readback is diagnostic, not an extra mandatory gate. SendInput/ACK alone is not consumption.
- Human login/consent, physical keyboard, special hardware/topology and visual assessment belong to a separate developer-selected manual catalog. Do not put them into automatic sequences, version-release requirements or post-run "remaining acceptance" requests. Never bypass security gates to make them automatic; never claim unperformed coverage.
- The release gate contains only unattended builds, automatic CTests, supported-version compatibility and package checks. Reuse unaffected evidence with artifact/configuration/environment provenance. A developer may choose extra manual assessment, but the agent does not make that a release condition.

## External Operation Guardrail (GitHub)
- Any operation that connects to GitHub (for example `gh auth`, `gh repo create`, `git push`, `git fetch`, `git pull`) must be announced to the user in advance.
- Agent must wait for explicit user confirmation after the announcement before executing GitHub-connected commands.

## Output Contract for Each Completed Task
- Code or doc changes made in the workspace.
- Build and stream evidence captured when possible.
- MODULE_KANBAN.md updated (task status + module Updated date).
- DEVLOG entry (brief summary, not per-file narrative).
