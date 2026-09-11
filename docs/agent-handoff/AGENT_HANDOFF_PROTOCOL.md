# Agent Handoff Protocol

## Purpose
Ensure any agent can continue work safely with minimal context loss.

## Mandatory Handoff Package
1. Current scope
- Module ID(s)
- Current milestone
- In-progress task IDs

2. Code and branch state
- Branch name
- Last commit hash
- Uncommitted change summary

3. Validation evidence
- Tests executed
- Pass/fail summary
- Known flaky tests

4. Open risks and blockers
- Technical risk list
- Pending decisions requiring owner input

5. Immediate next step
- Single actionable command/task the next agent should execute first

## File Update Requirements
- Append handoff note to `docs/logs/DEVLOG.md` (use Phase Summary Format).
- Update task and module status in `docs/runtime/MODULE_KANBAN.md`.
- If project stage changed, update `docs/runtime/PROJECT_STATE.md`.

## Handoff Template
```
[HANDOFF]
Date:
From agent:
To agent:
Modules:
Branch/commit:
Completed since last handoff:
Tests run and result:
Open risks/blockers:
Next immediate step:
```
