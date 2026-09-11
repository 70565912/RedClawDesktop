# Master Development Plan

Date: 2026-03-12
Planning baseline: v0.1

## Delivery Strategy
- Build in small modules with strict interface contracts.
- Each module must pass unit/integration tests before merge.
- Assemble validated modules into milestone builds.
- Run system-level test passes after each assembly milestone.

## Milestones

### M0: Project Skeleton and Tooling (Week 1)
- Repository structure, coding standards, CI skeleton.
- Build scripts, test runner, lint rules, artifact conventions.
- **Technical risk spikes (must complete before M1):**
  - `M01-T00`: libdatachannel peer-loopback PoC — validates P2P connectivity tech stack.
  - `M04-T00`: DDA minimum capture PoC — validates screen capture tech stack.
- Exit criteria:
  - CI runs unit tests on pull request.
  - Basic host/controller apps launch with mock transport.
  - Both tech spikes produce validated pass evidence.

### M1: Connectivity and Offline Signaling MVP (Weeks 2-4)
- ICE and candidate lifecycle.
- Offline offer/answer exchange (encrypted text + QR).
- Session bootstrap and secure channel establishment.
- Exit criteria:
  - Two hosts can connect over LAN and typical home NAT.
  - Direct/P2P and relay fallback state is observable in diagnostics.

### M2: Remote Desktop Core MVP (Weeks 5-8)
- Screen capture, encode/decode, render pipeline.
- Input injection, permission prompts, session controls.
- Windows DirectX game window capture compatibility path.
- Exit criteria:
  - Stable remote control at usable latency under normal network.
  - Remote display can show target DirectX game window output on supported Windows capture path.

### M3: Unattended Access and Install-as-Service (Weeks 9-10)
- Windows service install/start/upgrade/uninstall.
- Pre-login session support path and post-login helper handoff.
- Strict remote identity verification and policy-gated full-control activation.
- Remote UAC consent-screen view and confirmation capability after verification.
- Exit criteria:
  - Reboot machine, no user login, remote session can be initiated according to policy.
  - Verified full-control session can complete UAC confirmation remotely with audit logs.

### M4: Productivity Features (Weeks 11-13)
- Clipboard sync.
- File transfer with resume.
- Multi-display handling.
- Exit criteria:
  - Cross-display switch and file transfer recovery tested.

### M5: Hardening and Release Candidate (Weeks 14-16)
- Security hardening, fuzzing focus areas, resilience tests.
- Performance tuning and installer polish.
- Exit criteria:
  - Release checklist complete and signed off.

### M6: Mobile/Tablet Portable Companion (Weeks 17-20)
- Android-first mobile/tablet companion client for fragmented remote development usage.
- Secure status dashboard, command presets, notifications, and artifact/log inspection.
- Explicit no-controlled-input policy for mobile companion in this phase.
- mac portable companion and generic web mini client stay in planning-only deliverables.
- Exit criteria:
  - Android phone/tablet build can connect to host sessions under policy.
  - On-the-go workflows (view build/test status, trigger safe commands, inspect logs) pass acceptance tests.
  - mac/web planning docs are reviewed and linked for later phases.

## Workstream Parallelization
- Agent A: Connectivity + signaling stack.
- Agent B: Media pipeline.
- Agent C: Input/permissions + unattended service model.
- Agent D: Test automation + CI + tooling.
- Agent E: Mobile/tablet companion client and shared protocol bindings.

**Note on risk ordering:** Agents A and B should start with their respective spike tasks (`M01-T00`, `M04-T00`) before full module implementation. Agent D should prioritize `X00-T01` (CI pipeline) as it is a gate criterion for foundation→implementation.

## Definition of Done (per module)
- Design note updated.
- API contract documented.
- Unit tests implemented and passing.
- Integration tests implemented and passing.
- Security and error-path checks included.
- MODULE_KANBAN.md updated (task status + module Updated date).
- DEVLOG phase summary recorded.

## Reference Documents
- Coding conventions: `docs/setup/coding-conventions.md`
- Task tracking: `docs/runtime/MODULE_KANBAN.md`
- Stage gate criteria: `docs/runtime/PROJECT_STATE.md`
- Test scenarios: `docs/testing/test-matrix.md`
