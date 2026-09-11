# Contributing

## Workflow
- Default to delivery slices from `docs/runtime/PROJECT_STATE.md`.
- Current priority is dual-machine desktop image transfer and display, tracked as `P0-DS-*` in `docs/runtime/MODULE_KANBAN.md`.
- Use module assignments (`M01` ... `M11`) only when the user explicitly asks for module-scoped work.
- Follow `docs/runtime/AGENT_EXECUTION_PROTOCOL.md`.
- Keep changes scoped to the active delivery slice. Cross-module changes are allowed when they directly support the Host capture -> Controller display path.

## Validation Expectations
- Prefer build + focused runtime evidence over exhaustive module test sweeps.
- For desktop-stream work, useful evidence includes local two-process logs, frame counters, controller-side visible output, or two-machine run artifacts.
- Add or run fine-grained tests only when they protect a risky change or a concrete regression.

## Required Updates Per Task
- `docs/runtime/MODULE_KANBAN.md` (task status + module Updated date)
- `docs/logs/DEVLOG.md` (phase-level summary for significant milestones)

## Pull Request Expectations
- State completed slice/task IDs.
- Include build, focused test, or stream-run evidence.
- List residual risks.
- Suggest next delivery slice.
