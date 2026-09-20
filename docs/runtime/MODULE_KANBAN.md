# Module Kanban

Updated: 2026-09-20

This board tracks public project status. Detailed machine-specific evidence stays outside the repository. The current release checkpoint is the combined `v0.1.3` Developer Preview; the operator selected current main and superseded the separate v0.1.2-first plan on 2026-09-20.

Status values: `done`, `in-progress`, `planned`.

Validation routing follows the [automated-only release matrix](../testing/test-matrix.md). Human-assisted tests are excluded from automatic sequences and version-release requirements; the developer chooses any manual evaluation independently. Historical physical/native "pending" notes record unperformed coverage, not current release blockers or automatic follow-up tasks. Reclassification never claims those cases passed.

## M01 Capture

Updated: 2026-09-15

- `M01-T01` Windows Graphics Capture and Desktop Duplication backends — done.
- `M01-T02` Multi-display product controls — planned.

- `M01-T03` DDA recovery and Host cursor consistency — in-progress; implementation, serial Debug/Release builds, 102 CTests, focused session/GPU/protocol tests and isolated full-directory upgrade scenarios passed. Local DDA/WGC/GDI capture, both pre-upgrade Debug and public v0.1.0/v0.1.1 mixed directions, DHT restart and a matched-scene performance sample are recorded. First-frame capture denial now visibly opens the paused workspace with control disabled. Automatic qualification and publication remain pending; physical cursor/recovery and real peer rollout are optional developer evaluations, not release gates. See [capture behavior](../architecture/capture-recovery-and-cursor.md) and [independent upgrade](../testing/runtime-directory-upgrade.md).

## M02 Codec

- `M02-T01` H.264/HEVC encode/decode pipeline with hardware selection — done.
- `M02-T02` Broader hardware/driver compatibility matrix — in-progress.

## M03 Security

- `M03-T01` Identity, protected local secrets, and trust-store baseline — done.
- `M03-T02` Public security review and hardening — planned.

## M04 Protocol

- `M04-T01` Typed Control/Media/Agent framing and bounded parsing — done.
- `M04-T02` Compatibility/version policy for later public releases — planned.

## M05 Input

Updated: 2026-09-20

- `M05-T01` Authorized ordinary-desktop keyboard and mouse input — done.
- `M05-T02` Wider DPI/rotation/multi-display validation — in-progress.
- `M05-T03` Opt-in input-stage and native target delivery diagnostics — in-progress; Debug DHT build and seven focused suites passed. A physical temporary target visibly consumed a GUI click and QA-protocol digit; the peer aligned six received/sent batches, twelve injected events, zero rejected and an empty queue. The visible target response qualifies application consumption; separate peer readback is not required again. Physical-keyboard Hook capture is an optional developer evaluation; software-injected keys remain filtered and no manual follow-up is required. The target later disappeared and input was paused. Preserve these distinct transport, API and application evidence levels; the historical incident remains unresolved.

## M06 Session

- `M06-T01` Host/Controller lifecycle, reconnect, and latest-frame semantics — done.
- `M06-T02` Long-duration cross-site stability — planned.

## M07 Service

- `M07-T01` Windows service and pre-login handoff scaffolding — in-progress.
- `M07-T02` Signed unattended installer and upgrade — planned.

## M08 Diagnostics

- `M08-T01` Bounded status, redacted logs, and evidence export — done.
- `M08-T02` Public support-bundle workflow — planned.
- `M08-T03` Host media timestamp-pair diagnostics and clock-rate characterization — in-progress; single-slot diagnostics, offline Debug build and three focused CTests passed (13 adaptation cases, seven new). Earlier severe media-queue estimates caused sender throttling/admission rejection; physical clock drift versus genuine waiting remains unproven. The peer subsequently reported candidate validation, replacement/restart and about 20.51 FPS with queue 0–2 ms; this supersedes the former missing-key blocker but does not isolate a root-cause fix. Near-static blur persists, with a recorded 1920×1080 source and 1454×818 encode target. Current user authorization covers local publication and Host-role restart; collect normal baselines now and pair recurrence samples before recovery. See [diagnostic handoff](../testing/desktop-latency-quality-investigation-20260915.md). Endpoint-owned evidence and permission handling remain on X00-T13; do not declare the incident complete or duplicate existing monitors.

## M09 Render

- `M09-T01` D3D11 decoded-surface presentation with bounded CPU fallback — done.
- `M09-T02` Broader GPU/driver validation — in-progress.

## M10 UI

Updated: 2026-09-15

- `M10-T01` Device-code Host/Controller GUI and playback workspace — done.
- `M10-T02` Accessibility, localization, and product polish — in-progress.

## M11 Portable Client

- `M11-T01` Android-first portable client architecture — planned.

## X00 Cross-module delivery

Updated: 2026-09-20

Physical follow-up for `X00-T16` (2026-09-20): rebuilt/published Debug from `88a3803`; all 11 focused suites passed after three sandbox-sensitive normal-user reruns, with one opt-in paint case skipped. Public-DHT/ICE real H.264/D3D11 playback and one approved Cursor task passed. A target visibly consumed a GUI click and QA-protocol digit. Physical-keyboard capture, application paste and human-assisted workspace round trips are optional developer evaluations, not automatic follow-ups or release conditions. Source-derived capability versions still are not observed negotiation evidence. No peer restart/deployment occurred, and no full remote file/clipboard/terminal matrix is required.

- `X00-T01` Real local dual-GUI desktop stream with three channels — done.
- `X00-T02` Fixed ICE UDP port, early reservation, and ICE-targeted UPnP — done.
- `X00-T03` `v0.1.0` clean public repository and portable prerelease — done.
- `X00-T04` One physical cross-LAN public-DHT/direct-NAT real-video checkpoint — done; remote input, real Provider, mixed-version, and TURN coverage remain open.
- `X00-T05` Expanded physical cross-LAN and TURN acceptance — planned.
- `X00-T06` `v0.1.1` Agent login consistency and published-package cloud smoke — done.
- `X00-T07` GitHub unit baseline linkage and reusable vcpkg binary cache — done.
- `X00-T08` Agent message borders, activity sizing, conversation window and Cursor approval-expiry retry — done; shared Debug build and full UI suite passed. UI/layout acceptance is local; do not require a separate physical run of every layout case. Release publication remains a release-level check.
- `X00-T09` Cursor/Codex nested output, tool summaries, final fallback and duplicate filtering — done; local Provider/conversation regressions passed. The 2026-09-20 Cursor round trip supplies representative physical transport evidence, not every Provider combination; parser regressions stay local.
- `X00-T10` Task-scoped approval identity, bounded Agent transcript replay, and failed Codex turn recovery — in-progress; six focused local Agent/IPC suites and Debug publication passed; upgraded Controller/old Host reconnected and reused approval IDs executed successfully. Read-only UAC sampling/readback returned Default desktop with no consent/LogonUI in the shared session; historical input failure remains unresolved. Host-side deployment/flood acceptance remains open.

- `X00-T13` Remote Agent autonomous execution and result-only collaboration skill — done; repository skill and mandatory AGENTS entry define endpoint-local permission decisions, method selection, stage-scoped blockers and redacted result receipts without controller-side raw-environment collection or command micromanagement. This is a workflow-documentation change, not permission bypass, peer adoption, deployment or incident completion. See [skill](../../.agents/skills/remote-agent-result-contract/SKILL.md).
- `X00-T14` Local Agent sync rejection classification and authorization recovery — in-progress; implementation and offline qualification complete (Debug NoPublish, five focused CTests, four regression cases reproduced before fixes). Request rejections preserve durable task/approval state and ACKs; denied authorization suspends retries and recovery requires fresh sync. The operator now authorizes local build/publication and Host-role restart for continued work from another computer. Peer authorization remains endpoint-owned; physical direct-Agent and picture-quality acceptance are pending. See [diagnostic handoff](../testing/desktop-latency-quality-investigation-20260915.md) and [development log](../logs/DEVLOG.md).

- `X00-T12` Debug Controller startup on a physical adapter with multiple IPv4 addresses — done; Windows-selected gateway source address validated through the actual command launcher, Debug publication, public-DHT connection, remote real-capture rates, Controller frame deltas, and visual desktop confirmation. Broader exit-mode configuration remains on `X00-T11`.
- `X00-T15` Pending Agent approval during Provider output — in-progress; reproduced broker state replacement and disappearing GUI approval, fixed both endpoint emission and older-peer presentation, serial Debug/Release builds and four focused CTests passed. The final full sweep recorded one UI heartbeat failure (focused rerun passed) and a DDA access-denied skip. The independent local upgrade completed; peer reconnection and physical approval replay are optional developer evaluations, not release gates. No authorization or timeout relaxation.
- `X00-T16` Remote workspace v0.1.3 — in-progress; optional versioned file/clipboard/terminal channels, bounded transfer workers and shared mutation gates are implemented. Actual local online DHT/RTC windows passed upload/download hashes, empty directories, cancellation, progress/input gates, terminal Unicode output and received-copy list/open/cleanup. Navigation expands within layout above Agent; selected conversations follow new messages. Independent full-directory Host/Controller upgrades and terminal-issued Host restart completed; cold-start/rollback/parent-exit and repeated-ID history checks passed. Session-scoped frame accounting and terminal reconnect/output-ACK recovery have focused and native regressions. Final explicitly rebuilt full CTest: 112 passed, one DDA permission skip, zero failures; an earlier ICE expiry assertion failure remains recorded and passed both the isolated and final full reruns. Final serial Debug/Release builds, all five opt-in native terminal cases, actual Controller runtime reconnect with its Shell variable retained, and four mixed-version real-video combinations with v0.1.0/v0.1.1 passed. Local ZIP hashing, extracted command startup, missing-terminal rejection and extracted Release GUI real-video gates passed; the first cold-start attempt was blocked by local Agent readiness. Automatic package/release acceptance remains open. Matched concurrent-load performance is an engineering task; actual application Ctrl+V and physical peer upgrade are optional developer evaluations, not release gates. The operator selected the complete mainline as v0.1.3 on 2026-09-20, incorporating the unreleased v0.1.2 candidate; see the [approved contract](../architecture/remote-workspace-v013.md) and [development log](../logs/DEVLOG.md).

- `X00-T17` Automated-only test sequence and release policy — done; seven areas reuse 31 CTest targets with deduplication, guarded test-only builds, bounded serial runs and JSON/JUnit/case receipts. Twelve manual/native cases are excluded at CTest registration; three desktop probe targets remain developer-invoked only. The final affected 24-suite build/run passed with zero failures/skips. Self-regression checks selection, manual exclusions and result/exit classification. No app publication, role change or peer action. See [test matrix](../testing/test-matrix.md).
- `X00-T18` Optional future native-fixture automation — planned, non-blocking; a developer may later make additional clipboard/input scenarios unattended and then register them as automatic cases. Current physical-key/application/foreground evaluations are the developer's choice, not mandatory work, automatic skips or release conditions.

## Release gate

The Developer Preview requires successful builds, unattended CTest checks, a clean portable archive and automated local endpoint evidence for affected cross-component behavior. Reuse evidence only when relevant source, dependencies, configuration and artifact identity still match; do not repeat unchanged cases for each documentation or packaging edit. The product performance baseline remains visible but does not block feature delivery solely because a machine-specific target is missed.

Supported-version directions, rolling upgrade and reconnect remain automatic checks using local endpoint bundles. Release requires only unattended checks. No manual physical cross-LAN run, actual application paste, physical keyboard, UAC/account action or visual confirmation is required to release a version. These evaluations may be chosen by the developer and do not appear as pending automatic acceptance. Report only the coverage actually executed; physical deployment remains separate.
