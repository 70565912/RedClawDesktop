# Native modular refactoring

This is the module map and behavior-preservation contract, not a second status board.
Progress and evidence live in [PROJECT_STATE](../runtime/PROJECT_STATE.md),
[MODULE_KANBAN](../runtime/MODULE_KANBAN.md) and the
Validation is tracked by the current build and focused module tests.

## First delivery boundaries

| Previous owner | Extracted responsibility | New owner |
| --- | --- | --- |
| `src/main.cpp` | CLI parsing, profile precedence, validation, help | Qt-free `src/runtime/runtime_options*`, profile adapter and usage |
| `gui_shell.cpp` | Backend selection, D3D11/OpenGL/Qt presentation | `src/ui/playback/*_playback_canvas.cpp` and factory |
| `gui_shell.cpp` | Shared mapping/event, decoder thread, latest frame | `DirectFramePipeServer` in `ui/playback/direct_frame_receiver.*` |
| `net_module.cpp` | STUN readiness probe | Internal `stun_server_probe.*` |
| `net_module.cpp` | Diagnostic peer pairs | `tests/net/support/peer_harness.*`, test-only library |
| `video_frame_transport.cpp` | Wire parsing and reassembly | Remains in that file |
| `video_frame_transport.cpp` | Navigation wire, metadata, feedback/estimation, congestion, pacing budget, send worker, receiver policy | Separate named `.cpp` files in `src/net/src` |
| `diag_module.cpp` | Runtime status extraction | `runtime_status_parser.cpp` |
| `tests/CMakeLists.txt` | Per-module test declarations | `tests/cmake/*-tests.cmake`, included in the original directory scope |

`src/runtime` currently contains application-internal startup components, not a
new protocol/session framework. Its small contract is reusable by CLI tests without Qt.
The playback receiver's PImpl owns exactly the existing resources; its destructor
joins the existing worker before removing callbacks. The Qt parent owns each canvas
widget, and `PlaybackWidgetResult::canvas` aliases that widget. No second playback
queue, worker or background probe was introduced.

`media_transport_limits.h` shares immutable wire overhead and in-flight timing
helpers. It must not become another congestion authority. The estimator still owns
in-flight accounting; the pacer still owns its two-slot scheduling and recovery state.

## Invariants

- No wire-format change, candidate/description ordering change, DHT instance or
  request-tag relaxation, retry-policy change, or credential/logging change.
- Preserve two-slot latest-only, no generic retransmission, no B frames, requested
  resolution and clarity floor. Recovery IDR's 10-second budget is unchanged.
- Preserve parser defaults, GUI pass-through arguments, profile-before-CLI precedence,
  local authorization defaults, backend priority and geometry transaction behavior.
- `include()` is intentional in test registration: `add_subdirectory()` would require
  changing `DIRECTORY TESTS` aggregation and could silently remove test build targets.
- Test-only peer pairs are not a product fallback transport and are no longer declared
  by the production network header.
- File extraction is not evidence of faster FPS, lower latency, lower CPU or a repaired
  cross-LAN connection. Those claims require matched measurements/real peer evidence.

## Remaining sequence

1. Characterize and extract GUI process/QA/evidence ownership from `launch_gui_shell`.
2. Extract runtime signaling, Host stream and Controller receive owners, preserving
   retirement/drain before reset. Do not replace the function with a giant shared Context.
3. Separate capture device/scaler/backend/encoder ownership and render decoder/pool code.
4. Continue DHT codec/store/publisher, service/session, Agent and large test/script files.
5. Reduce umbrella headers once consumers have typed dependencies; remove obsolete paths.

The first delivery does **not** finish the repository-wide refactor. Large runtime and
GUI orchestration remain explicit follow-up work, not newly renamed giant files.
Use focused tests after each owner extraction and full rebuilt unit gates at publication.
Retain Windows guards and optional Qt/OpenGL paths. Main application builds use
`build.ps1`; deploy only to the fixed `release/<Configuration>` path.
