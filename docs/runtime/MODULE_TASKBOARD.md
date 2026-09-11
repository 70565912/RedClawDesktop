# Module Taskboard

> **DEPRECATED (2026-03-20):** This file has been superseded by `docs/runtime/MODULE_KANBAN.md`.
> Task tracking and module status are now unified in a single kanban board with dependency tracking.
> This file is preserved for historical reference only. Do not update it.

---

**Historical snapshot (2026-03-20):**

Legend: `todo` | `in-progress` | `blocked` | `done`

## M01
- M01-T01: ICE wrapper skeleton and state callbacks [todo]
- M01-T02: Candidate parse/validate unit tests [todo]
- M01-T03: LAN peer integration test harness [todo]

## M02
- M02-T01: Offer blob schema v1 definition [done]
- M02-T02: Encrypt/decrypt blob implementation [done]
- M02-T03: QR serialize/deserialize helper [done]

## M03
- M03-T01: Handshake message model and nonce policy [in-progress]
- M03-T02: Replay protection validator [todo]
- M03-T03: Peer fingerprint verification path [todo]

## M04
- M04-T01: Windows capture abstraction skeleton [todo]
- M04-T02: Encoder config low-latency profile [todo]
- M04-T03: 30-min stability test script [todo]
- M04-T04: DirectX game window capture compatibility and fallback metrics [todo]

## M05
- M05-T01: Decoder abstraction and frame queue [todo]
- M05-T02: Render path viewport controls [todo]
- M05-T03: Resolution switch integration test [todo]

## M06
- M06-T01: Input policy gate model [todo]
- M06-T02: Keyboard/mouse injection adapter [in-progress]
	note: runtime adapter abstraction, Windows SendInput backend, dedicated Windows secure-desktop backend, user/secure target routing adapter, capability-driven runtime controller, session-level orchestrator (capability + channel readiness + lifecycle signal gating), and host lifecycle dispatcher landed; secure-desktop OS path hardening pending.
- M06-T03: Policy deny path test suite [in-progress]
	note: adapter deny/allow/backend-fail, invalid-event validation, target-routing, and capability-runtime integration tests landed; expand for real injection backend error surfaces.
- M06-T04: Secure-desktop UAC input policy gate [in-progress]
	note: policy gate implemented, cross-module linkage and runtime-control integration tests added, secure backend gate plumbing landed, app-entry wiring path established, dedicated secure backend channel gate and channel-readiness orchestrator API landed, lifecycle event-source abstraction/dispatcher binding (`IHostServiceLifecycleEventSource`, `CallbackDrivenHostServiceLifecycleEventSource`, `bind_host_service_lifecycle_event_source`) landed, runtime bridge (`HostServiceRuntimeLifecycleBridge`) couples service wrapper start/stop with lifecycle fail-close sequencing, and orchestrator sequence hardening coverage now includes `ready->disconnect->ready`, `disconnect->ready` blocked-until-fresh-regrant, and `ready->lost->ready`; dedicated UAC transport/session binding beyond SendInput reuse remains pending.

## M07
- M07-T01: Windows service install/start/stop wrapper [in-progress]
	note: Windows service lifecycle wrapper with install/start/stop/uninstall command execution and tests landed; installer project scaffold (WiX + build script + CMake targets) and installer CI scaffold workflow added; replace shell executor with SCM-native adapter next.
- M07-T02: Boot-time host startup validation [todo]
- M07-T03: Pre-login to user-session handoff flow [todo]
- M07-T04: Privileged control verification and remote UAC confirmation broker [in-progress]
	note: broker hooks and capability-change callbacks implemented, session orchestrator consumes capability callbacks for input runtime gating, lifecycle dispatcher bridge landed, disconnect-to-broker revoke teardown linkage landed, and service event-source emitter integration landed; OS-level UAC integration pending.

## M08
- M08-T01: Clipboard text sync protocol [todo]
- M08-T02: File transfer chunking with resume token [todo]
- M08-T03: Resume checksum integration tests [todo]

## M09
- M09-T01: Session state enum and transition matrix [in-progress]
- M09-T02: Fault recovery transitions [todo]
- M09-T03: Illegal transition test coverage [todo]

## M10
- M10-T01: Structured logging schema and redaction [todo]
- M10-T02: Connectivity diagnostics snapshot [todo]
- M10-T03: Support bundle exporter [todo]

## M11
- M11-T01: Android portable session model and no-controlled-input capability policy [todo]
- M11-T02: Android status dashboard and artifact/log fetch flow [todo]
- M11-T03: Guarded command preset flow + mac/web mini access planning docs [todo]
