# Docs Directory Agent Guide

Applies to `docs/**`.

## Entry Model

- `README.md` at repo root is the single mandatory onboarding entry.
- `docs/README.md` is an optional extended navigation index.
- New or moved documents must be reachable from the root README through explicit links, either directly or through the nearest parent index.

## Runtime Documents

- `docs/runtime/PROJECT_STATE.md` is the source of current stage, focus modules, blockers, and next resume point.
- `docs/runtime/MODULE_KANBAN.md` is the single source for module task status.
- `docs/runtime/MODULE_PLAYBOOKS/<module>.md` stores module-specific acceptance criteria and tests.
- `docs/logs/DEVLOG.md` stores phase-level summaries and handoff notes.

## Documentation Change Rules

- Avoid orphan documents. Add parent index links for every new doc.
- Preserve README-first discoverability.
- Keep terminology aligned with `docs/architecture/solution-architecture.md` and module playbooks.
- Planning docs should include scope, milestones, dependencies, risks, and acceptance criteria.
- Do not duplicate status across multiple docs; link to the single source instead.

## Task Completion Docs

For completed module tasks, update:

- `docs/runtime/MODULE_KANBAN.md`: task status and module Updated date.
- `docs/logs/DEVLOG.md`: concise milestone summary.
- `docs/runtime/PROJECT_STATE.md`: only when stage, blockers, focus modules, or next resume point changed.

## Handoff

Use `docs/agent-handoff/AGENT_HANDOFF_PROTOCOL.md` for handoff blocks. Include branch/commit, uncommitted changes summary, validation evidence, open risks, and one immediate next step.
