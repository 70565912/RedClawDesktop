# Desktop GUI Framework and Delivery Plan

Date: 2026-04-02
Scope: Windows-first desktop UI baseline for `redclaw_desktop.exe`

## Decision Summary
- Selected framework: Qt 6 (Widgets first, optional QML later).
- Primary reason: best fit with current project constraints in README/architecture:
  - Mandatory desktop UI deliverable (not CLI-only).
  - Windows-first native integration and long-running host/controller workflows.
  - Existing C++ codebase and CMake build system.
  - Installer-required delivery path (service lifecycle + UI coexistence).

## Why Qt Is the Best Fit Here
1. Engineering alignment
- Current codebase is C++ and already organized by runtime modules (`M01`..`M11`, `X00`).
- Qt keeps UI in the same language/runtime, reducing bridge complexity and duplicated models.

2. Build and toolchain compatibility
- Project baseline already states Qt in architecture and toolchain notes.
- Qt + CMake + Ninja + VS2022 is a known, stable Windows path.

3. Product requirement fit
- Required UI surface (session create/join, role selection, connection state, actionable errors) maps cleanly to Qt Widgets.
- Future expansion (multi-pane diagnostics, timeline views, status cards) is straightforward.

4. Installer and operations fit
- Windows installer delivery (install/upgrade/uninstall + service registration lifecycle) is easier with native Qt app packaging than web-runtime embedded stacks.

## Alternatives Considered (and Why Not Primary)
- WinUI 3 / WPF:
  - Good native UX, but introduces .NET/C++ boundary complexity and mixed stack overhead.
- Electron/Tauri:
  - Faster UI iteration, but adds web stack/runtime distribution complexity and extra process model not ideal for current C++ runtime integration stage.
- Dear ImGui:
  - Great for tooling/debug UI, but not ideal as product-grade end-user desktop UX baseline.

## Delivery Strategy

### Phase G1 - UI Shell Baseline (1 week)
Goal: launch graphical shell instead of CLI-only experience for default desktop mode.

Deliverables:
- `QApplication` entry path and main window shell.
- Top-level navigation tabs/panels:
  - Session
  - Runtime Status
  - Diagnostics
- CLI mode preserved behind explicit flag (for automation/smoke).

Acceptance:
- Launching `redclaw_desktop.exe` without role-only args opens GUI shell.
- Existing CLI role mode still works with explicit runtime flags.

### Phase G2 - Session Workflow UI (1 week)
Goal: meet mandatory session UX baseline.

Deliverables:
- Session Create/Join forms:
  - role selector (`host`/`controller`)
  - signaling transport selector (`file`/`event-log`/`tcp`)
  - target host/port and timeout fields
  - ICE server list editor
- Start/Stop runtime controls.
- Real-time connection state and failure banner.

Acceptance:
- User can start host/controller flows from GUI without terminal commands.
- Connection/failure state transitions are visible and actionable.

### Phase G3 - Timeline and Diagnostics Binding (1 week)
Goal: connect existing UI model abstractions to actual widgets.

Deliverables:
- Bind existing render timeline model to Qt list/table widgets.
- Severity color coding (`info/warning/error`) and filter controls.
- Candidate/network diagnostics panel (state, candidate stats, TCP reconnect metrics).

Acceptance:
- Runtime status timeline shown in GUI with live updates.
- No longer relies on stdout text as primary operator UI.

### Phase G4 - Installer and Service UX Integration (1 week)
Goal: align GUI with mandatory installer/service delivery.

Deliverables:
- Service lifecycle panel:
  - install/start/stop/uninstall status display
  - privilege-required action prompts
- Packaging and installer integration checks (Qt runtime deployment included).

Acceptance:
- Fresh machine install can launch GUI and manage service baseline flows.

## Architecture and Code Organization Plan
- New module path (proposed):
  - `src/ui/` (Qt window, view models, controller adapters)
- Keep runtime/business modules UI-agnostic:
  - `src/net`, `src/session`, `src/render`, `src/service` remain core logic.
- Introduce adapter layer from runtime events to Qt signals/slots:
  - avoid direct UI references in core modules.

## Test Plan
- Unit tests:
  - UI view-model mapping tests (state conversion, severity mapping, validation rules).
- Integration tests:
  - start/stop runtime from UI controller adapter.
  - role workflow smoke (`host` and `controller`) with mock signaling endpoints.
- Manual acceptance:
  - run checklist for session create/join, error recovery, diagnostics visibility.

## Risks and Mitigations
- Risk: event loop/threading conflicts between runtime callbacks and UI thread.
  - Mitigation: strict signal/slot queued connections + thread-safe dispatcher boundary.
- Risk: keeping CLI and GUI entry paths diverging.
  - Mitigation: single runtime controller service used by both CLI and GUI frontends.
- Risk: installer packaging misses Qt runtime artifacts.
  - Mitigation: add packaging verification step in installer build pipeline.

## Milestones and Exit Criteria
- M-GUI-01: GUI shell launches and coexists with CLI mode.
- M-GUI-02: session create/join role workflow fully operable via GUI.
- M-GUI-03: timeline and diagnostics visible in GUI as primary operator surface.
- M-GUI-04: installer-integrated GUI delivery validated on clean machine.

## Immediate Next Actions
1. Add `src/ui` CMake target and Qt dependency wiring.
2. Implement `QApplication` + `MainWindow` shell and keep CLI fallback behind explicit flag.
3. Build runtime-controller adapter that consumes existing runtime timeline/state events.
4. Deliver first end-to-end GUI host startup smoke and record evidence in DEVLOG.
