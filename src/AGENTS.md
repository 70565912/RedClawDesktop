# Source Directory Agent Guide

Applies to `src/**`.

## Architecture Shape

Native code is organized as C++20 CMake modules. Each module should follow:

```text
src/<module>/
  include/redclaw/<module>/<module>_module.h
  src/<module>_module.cpp
  CMakeLists.txt
```

Every module builds as a `STATIC` library unless the existing CMake target says otherwise. Public headers live under `include/redclaw/<module>/`.

Current module surface:

- `core`: shared core primitives and future security constants.
- `protocol`: versioned schemas, serializers, parsers, validation, sealed offer payloads.
- `net`: WebRTC/libdatachannel and connectivity contracts.
- `security`: replay guard, fingerprint verification, trust policy.
- `session`: session state, lifecycle, recovery, handoff orchestration.
- `capture`: Windows capture and encoder bridge.
- `render`: decode queues, viewport controls, runtime status UI model.
- `input`: input policy and injection adapters.
- `service`: Windows service, IPC, capability sync, privileged broker.
- `helper`: helper process bootstrap and runtime profile parsing.
- `diag`: structured logs, diagnostics, support bundle export.
- root `src`: app entrypoints, GUI shell, host service entrypoint.

## C++ Style

- Follow `docs/setup/coding-conventions.md`.
- Prefer 4-space indentation for C++ edits and match surrounding style when touching existing code.
- Use `snake_case` for variables, functions, and namespaces.
- Use `PascalCase` for types/classes and `kPascalCase` for enum values in newly designed APIs.
- Keep public contracts small, typed, and validated at boundaries.
- Add comments only when they explain non-obvious control flow, security decisions, or platform constraints.

## CMake Rules

- Add sources to the owning module `CMakeLists.txt`.
- Use `target_compile_features(<target> PUBLIC cxx_std_20)` for module libraries.
- Apply `redclaw_apply_warnings(<target>)` to every new target.
- Link only explicit dependencies via `target_link_libraries`.
- Keep optional dependencies optional. Qt GUI code must remain behind `REDCLAW_ENABLE_QT_GUI` or the existing Qt availability checks.

## Dependency Boundaries

- Do not reach across modules without a CMake dependency and an intentional public header contract.
- Prefer dependency injection interfaces for cross-module services, especially service, input, session, and capture paths.
- Protocol and config parsing should use structured validators and result types such as `ParseResult<T>`.
- Avoid global mutable state in runtime/session paths unless there is an explicit lifecycle owner.

## Windows and Privilege Rules

- Guard Windows-specific code with `_WIN32`.
- Privileged control paths must fail closed when service state, helper capability, IPC readiness, consent token, or secure desktop detection is uncertain.
- Do not log raw secrets, passphrases, private runtime config values, or unredacted diagnostic payloads.
- Keep deterministic logic testable without admin rights; put OS integration behind adapters.

## Runtime Entrypoint Rules

- `redclaw_desktop` must retain CLI/runtime utility even when GUI support is unavailable.
- `redclaw_host_service` is the service-mode entrypoint; service changes should consider install/start/stop/uninstall behavior.
- Online rendezvous signaling is the formal product path for no-IP pairing; file/event-log/tcp transports are diagnostics or fallback paths unless re-scoped.
