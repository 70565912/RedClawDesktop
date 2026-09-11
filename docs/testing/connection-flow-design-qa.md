# Connection flow design QA

## Scope

Review the pre-connection, expanded network settings, Host waiting, Controller connecting, recoverable failure, and fatal failure states at the supported minimum and default window sizes.

The implementation lives in `src/ui/connection_entry_page.cpp`, `src/ui/connection_progress_page.cpp`, and `src/ui/gui_shell.cpp`.

## Acceptance criteria

- The four connection stages remain horizontal and readable without horizontal scrolling.
- Every stage exposes a text state such as Pending, Current, Complete, or Failed; color is supplemental.
- Local machine code is visibly read-only and the peer machine code is visibly editable.
- Expanding network settings reveals the complete ICE UDP port, STUN/TURN, and UPnP controls without clipping.
- Runtime Log remains outside the switching page area and retains readable content in every connection state.
- Retry and Exit actions are visible for terminal failures; automatic repair states do not ask the user to retry prematurely.
- Keyboard focus, disabled controls, scroll bars, margins, and bottom borders remain visible at minimum size and common DPI scales.
- Host and Controller copy fits without changing the underlying four-stage geometry.

## Evidence

Capture the required states with the repository's UI test or QA tooling. Store screenshots and comparison notes below the ignored `build/reports/` directory. Record logical size, physical size, device-pixel ratio, build configuration, commit, and the final issue severity list in the local report.

The review passes only when no open P0, P1, or P2 visual defect remains in the required states.
