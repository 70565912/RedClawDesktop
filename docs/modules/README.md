# Module Breakdown

Each module has:
- Scope and public interfaces.
- Implementation checklist.
- Test samples and acceptance criteria.

Execution documents:
- `docs/runtime/MODULE_KANBAN.md` for unified task + module status tracking.
- `docs/runtime/MODULE_PLAYBOOKS/` for per-module start instructions.

## Modules
- M01 Connectivity Engine (ICE/STUN/TURN)
- M02 Offline Signaling Exchange
- M03 Secure Session and Identity
- M04 Screen Capture and Encode
- M05 Decode and Renderer
- M06 Input Injection and Permission Control
- M07 Unattended Service and Boot-Time Host
- M08 Clipboard and File Transfer
- M09 Session Orchestration and State Machine
- M10 Diagnostics, Metrics, and Logging
- M11 Mobile/Tablet Portable Companion

## How To Start Any Module
1. Read `docs/runtime/PROJECT_STATE.md`.
2. Open `docs/runtime/MODULE_KANBAN.md` and find your module section.
3. Pick first `todo` task whose dependencies are satisfied.
4. Read `docs/runtime/MODULE_PLAYBOOKS/<module>.md` for acceptance criteria.
5. Execute task, run listed tests.
6. Update task status in `docs/runtime/MODULE_KANBAN.md`.
