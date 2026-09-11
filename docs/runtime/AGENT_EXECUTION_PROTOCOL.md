# Agent Execution Protocol (E2E-First)

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
- `docs/testing/test-matrix.md` — priority scenarios and KPI targets.
- `docs/setup/coding-conventions.md` — naming, formatting, testing framework, security constants.
- `docs/logs/DEVLOG.md` (latest phase summary only) — recent decisions and risks.

## Start-Work Algorithm
1. Read `docs/runtime/PROJECT_STATE.md`.
2. Select the first unfinished `P0-DS-*` item from the active priority queue unless the user explicitly names another task.
3. Identify the smallest vertical slice that moves the desktop-stream path forward.
4. Modify all modules needed for that slice; do not split the work only to preserve old module boundaries.
5. Prefer runnable behavior over broad test coverage: build, run focused checks, then capture stream evidence.
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
- Minimum validation for implementation slices: successful configure/build of affected runtime targets.
- Preferred validation: local two-process run with frame counters and controller-side visible output.
- Strong validation: two-machine run with host/controller logs and controller screenshot/photo or equivalent operator observation.
- Focused tests are encouraged for risky parser/state changes, but broad module test sweeps are deferred until after the first visual proof or release-style checkpoint.

## External Operation Guardrail (GitHub)
- Any operation that connects to GitHub (for example `gh auth`, `gh repo create`, `git push`, `git fetch`, `git pull`) must be announced to the user in advance.
- Agent must wait for explicit user confirmation after the announcement before executing GitHub-connected commands.

## Output Contract for Each Completed Task
- Code or doc changes made in the workspace.
- Build and stream evidence captured when possible.
- MODULE_KANBAN.md updated (task status + module Updated date).
- DEVLOG entry (brief summary, not per-file narrative).
