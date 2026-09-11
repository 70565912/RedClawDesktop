# Delivery Slice Assignment Contract

The default assignment unit is now a delivery slice, not a module. Use this minimal payload:

```
slice: P0-DS-xx
goal: <one sentence>
constraints: <optional>
```

Example:
```
slice: P0-DS-01
goal: Prove a local two-process Host-to-Controller desktop frame stream with visible controller output.
```

Module-scoped payloads are still allowed when the user explicitly wants them:

```
module: Mxx
goal: <one sentence>
```

## What the Agent Must Return
- Completed slice or task IDs.
- Files changed.
- Build, focused test, or stream-run evidence.
- Remaining risks.
- Next recommended slice.

## Task Selection Rules
- Active priority queue in `docs/runtime/PROJECT_STATE.md` overrides module task order.
- Pick the first unfinished `P0-DS-*` item unless the user names another task.
- Use `docs/runtime/MODULE_KANBAN.md` as the status ledger and place to record blockers, not as a strict module-by-module execution order during the desktop-stream push.
- If no ready delivery slice exists, create a planning slice `P0-DS-<n>` and record why.

## Completion Rules
A delivery slice is complete only if:
- it moves the Host capture -> Controller display path forward,
- build or focused validation evidence is recorded,
- stream-run evidence is captured when the slice reaches runtime behavior,
- `MODULE_KANBAN.md` is updated with status/blocker notes,
- `DEVLOG` summary entry is appended.

Do not block a delivery slice on exhaustive module tests unless the slice changes broad shared behavior or is approaching a release-style checkpoint.
