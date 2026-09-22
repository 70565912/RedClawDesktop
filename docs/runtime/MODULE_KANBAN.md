# Module Kanban

Updated: 2026-09-22

This board tracks public project status. Detailed machine-specific evidence stays outside the repository. The current release checkpoint is the `v0.1.4` Developer Preview. v0.1.3 remains the 2026-09-20 combined package; the operator selected current main and superseded the separate v0.1.2-first plan on 2026-09-20.

Status values: `done`, `in-progress`, `planned`.

Validation routing follows the [automated-only release matrix](../testing/test-matrix.md). Human-assisted tests are excluded from automatic sequences and version-release requirements; the developer chooses any manual evaluation independently. Historical physical/native "pending" notes record unperformed coverage, not current release blockers or automatic follow-up tasks. Reclassification never claims those cases passed.

## M01 Capture

Updated: 2026-09-21

- `M01-T01` Windows Graphics Capture and Desktop Duplication backends — done.
- `M01-T02` Multi-display product controls — planned.
- `M01-T04` Initial full-desktop capture selection — done (local candidate); normalized selection resolves against actual frame dimensions, including resolution recovery and crop rollback. Two focused automatic cases, Debug build/publication, published startup and artifact equality passed. No live Host replacement or physical field requalification; see the development log.
- `M01-T03` DDA recovery and Host cursor consistency — done; implementation, serial Debug/Release builds, 102 CTests, focused session/GPU/protocol tests and isolated full-directory upgrade scenarios passed. Local DDA/WGC/GDI capture, both pre-upgrade Debug and public v0.1.0/v0.1.1 mixed directions, DHT restart and a matched-scene performance sample are recorded. First-frame capture denial now visibly opens the paused workspace with control disabled. Released in the combined v0.1.3 package; local and published-package smoke passed; physical cursor/recovery and real peer rollout are optional developer evaluations, not release gates. See [capture behavior](../architecture/capture-recovery-and-cursor.md) and [independent upgrade](../testing/runtime-directory-upgrade.md).

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

Updated: 2026-09-22

- `M05-T01` Authorized ordinary-desktop keyboard and mouse input — done.
- `M05-T02` Wider DPI/rotation/multi-display validation — in-progress. Host injection now uses per-monitor DPI awareness v2 for each `SendInput` batch so absolute positions match physical capture pixels; the wire protocol is unchanged and a Host update is sufficient. Rotation and multi-display field tracking are not requalified, and the peer Host was not replaced with this binary. See [ordinary desktop input](../architecture/ordinary-desktop-remote-input-v1.md).
- `M05-T04` Remote chord ordering and remote clipboard shortcuts — done (local candidate); pending input precedes state snapshots, and remote copy/cut keeps subsequent paste remote until the local clipboard changes. Seventeen focused input/UI/navigation cases, Debug build/publication and startup passed. Running Controller preserved; physical keyboard/application consumption has not been requalified.
- `M05-T03` Opt-in input-stage and native target delivery diagnostics — in-progress; Debug DHT build and seven focused suites passed. A physical temporary target visibly consumed a GUI click and QA-protocol digit; the peer aligned six received/sent batches, twelve injected events, zero rejected and an empty queue. The visible target response qualifies application consumption; separate peer readback is not required again. Physical-keyboard Hook capture is an optional developer evaluation; software-injected keys remain filtered and no manual follow-up is required. The target later disappeared and input was paused. Preserve these distinct transport, API and application evidence levels; the historical incident remains unresolved.

## M06 Session

- `M06-T01` Host/Controller lifecycle, reconnect, and latest-frame semantics — done.
- `M06-T02` Long-duration cross-site stability — planned.

## M07 Service

- `M07-T01` Windows service and pre-login handoff scaffolding — in-progress.
- `M07-T02` Signed unattended installer and upgrade — planned.

## M08 Diagnostics

Updated: 2026-09-20

- `M08-T01` Bounded status, redacted logs, and evidence export — done.
- `M08-T02` Public support-bundle workflow — planned.
- `M08-T03` Host media timestamp-pair diagnostics and clock-rate characterization — in-progress; prior single-slot diagnostics and three focused CTests remain valid. X00-T20 now records same-instance long-session throttling and a locally verified clock-rate compensation fix; physical oscillator skew versus slow application/network drift is not uniquely identified, and live rollout remains separate. Earlier peer-reported restart recovery and near-static blur remain historical observations, not root-cause closure. See [diagnostic handoff](../testing/desktop-latency-quality-investigation-20260915.md). Endpoint-owned evidence and permission handling remain on X00-T13; do not declare the incident complete or duplicate existing monitors.

## M09 Render

- `M09-T01` D3D11 decoded-surface presentation with bounded CPU fallback — done.
- `M09-T02` Broader GPU/driver validation — in-progress.

## M10 UI

Updated: 2026-09-22

- `M10-T01` Device-code Host/Controller GUI and playback workspace — done. The playback window titled `RedClaw` is an independent top-level window, not owned by `RedClaw Desktop`. One focused lifecycle case passed. See [ordinary desktop input](../architecture/ordinary-desktop-remote-input-v1.md).
- `M10-T02` Accessibility, localization, and product polish — in-progress.
- `M10-T03` Controller floating task workspace — done; four owned task windows, movable translucent task bar, local layout persistence, hide-only lifecycle and input isolation. Debug NoPublish build and 14 focused unattended cases passed, including a corrected initial-layout timing fixture's single-case recheck. Unaffected tests were not rerun; native visual/physical-keyboard/hardware evaluation remains optional. Working-tree delivery only: no publication, peer upgrade or session restart. See the [workspace presentation contract](../architecture/remote-workspace-v013.md#floating-presentation-revision-m10-t03).

## M11 Portable Client

- `M11-T01` Android-first portable client architecture — planned.

## X00 Cross-module delivery

Updated: 2026-09-22

- `X00-T24` Compact connection homepage and password input — done; two aligned columns, single-entry asynchronous save, independent Show/Hide with asterisk masking, dirty-password waiting gate and default-collapsed permissions/Agent/network settings. Native Windows connection UI/flow tests passed at both standard and high DPI (19 cases each); 800×600 and 900×680 fixture screenshots captured. Initial Debug candidate published separately; subsequently started the local Host under operator authorization. Synchronized through upstream `6b6e930`; merged Debug build and 49 focused UI/runtime/input cases passed. Source delivery preserves that running Host and existing public release assets. See [connection instructions](../architecture/connection-password-v1.md) and [validation details](../logs/DEVLOG.md).

- `X00-T23` Mandatory connection password — done locally; DPAPI credentials, SCRAM-based all-channel admission, automatic rejection/acceptance, timeout/cooldown and retry recovery implemented. Debug build/publication, 104 focused cases, real local dual-GUI desktop, actual old-binary rejection in both directions, same-Host online bad-then-good recovery and credential privacy checks passed. Existing running instances and release assets unchanged; physical peer qualification remains separate. See [connection password contract](../architecture/connection-password-v1.md) and [evidence](../logs/DEVLOG.md).

- `X00-T22` Agent tasks and commands without repeated approval — done; Cursor launches submitted tasks/follow-ups directly; Codex applies `never` on start/resume/turn with the existing workspace sandbox. Current providers advertise no approval prompts; Agent v1 legacy-peer handling remains intact. Provider/broker/UI regressions passed 63/63; Debug publication validation is recorded in the [development log](../logs/DEVLOG.md). No live peer replacement or real-account execution was performed.

- `X00-T21` Shared local workspace control API — done; owner-user IPC (default on in Debug, opt-in in Release) and PowerShell client share GUI terminal/transfer state, bounded results and negotiated terminal 2 / clipboard 3 semantics. Earlier Debug build, 22 automatic suites plus runtime-options coverage, real API/file round trips, both mixed-version directions and online Host rolling upgrade passed. Source integration with the five Host-side commits through `2d9fbca` also passed its Debug main build and nine focused regression suites. The Debug-default follow-up passed its Debug main build and 15 runtime-options cases in each build configuration; broader Debug execution failed in unmodified LocalControlOutput/Protobuf coverage and remains unresolved. Independent candidate retains its earlier identity; running user instance preserved, default-listener live validation not performed. Concurrent-load display-rate reduction remains recorded for follow-up. See [contract and examples](../architecture/workspace-control-v1.md).

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
- `X00-T11` Host/Controller System default, selected-adapter, and system-proxy exit configuration — planned; code/log analysis and acceptance scope documented.
- `X00-T12` Debug Controller startup on a physical adapter with multiple IPv4 addresses — done; Windows-selected gateway source address validated through the actual command launcher, Debug publication, public-DHT connection, remote real-capture rates, Controller frame deltas, and visual desktop confirmation. Broader exit-mode configuration remains on `X00-T11`.
- `X00-T13` Remote Agent autonomous execution and result-only collaboration skill — done; repository skill and mandatory AGENTS entry define endpoint-local permission decisions, method selection, stage-scoped blockers and redacted result receipts without controller-side raw-environment collection or command micromanagement. This is a workflow-documentation change, not permission bypass, peer adoption, deployment or incident completion. See [skill](../../.agents/skills/remote-agent-result-contract/SKILL.md).

- `X00-T14` Local Agent sync rejection classification and authorization recovery — in-progress; implementation and offline qualification complete (Debug NoPublish, five focused CTests, four regression cases reproduced before fixes). Request rejections preserve durable task/approval state and ACKs; denied authorization suspends retries and recovery requires fresh sync. The operator now authorizes local build/publication and Host-role restart for continued work from another computer. Peer authorization remains endpoint-owned; physical direct-Agent and picture-quality acceptance are pending. See [diagnostic handoff](../testing/desktop-latency-quality-investigation-20260915.md) and [development log](../logs/DEVLOG.md).

- `X00-T15` Pending Agent approval during Provider output — in-progress; reproduced broker state replacement and disappearing GUI approval, fixed both endpoint emission and older-peer presentation, serial Debug/Release builds and four focused CTests passed. The final full sweep recorded one UI heartbeat failure (focused rerun passed) and a DDA access-denied skip. The independent local upgrade completed; peer reconnection and physical approval replay are optional developer evaluations, not release gates. No authorization or timeout relaxation.
- `X00-T16` Remote workspace v0.1.3 — done; optional versioned file/clipboard/terminal channels, bounded transfer workers and shared mutation gates are implemented. Actual local online DHT/RTC windows passed upload/download hashes, empty directories, cancellation, progress/input gates, terminal Unicode output and received-copy list/open/cleanup. Navigation expands within layout above Agent; selected conversations follow new messages. Independent full-directory Host/Controller upgrades and terminal-issued Host restart completed; cold-start/rollback/parent-exit and repeated-ID history checks passed. Session-scoped frame accounting and terminal reconnect/output-ACK recovery have focused and native regressions. Historical explicitly rebuilt full CTest: 112 passed, one DDA permission skip, zero failures; an earlier ICE expiry assertion failure remains recorded and passed both the isolated and final full reruns. Final serial Debug/Release builds, all five opt-in native terminal cases, actual Controller runtime reconnect with its Shell variable retained, and four mixed-version real-video combinations with v0.1.0/v0.1.1 passed. Local ZIP hashing, extracted command startup, missing-terminal rejection and extracted Release GUI real-video gates passed; the first cold-start attempt was blocked by local Agent readiness. Published v0.1.3 from d7b022e; local package extraction/startup, remote asset SHA256 and published-package Windows smoke passed. The redundant 2026-09-20 Release sweep was cancelled on operator instruction after DHT bind 10013 failures; it is not claimed as passed. Matched concurrent-load performance is an engineering task; actual application Ctrl+V and physical peer upgrade are optional developer evaluations, not release gates. The operator selected the complete mainline as v0.1.3 on 2026-09-20, incorporating the unreleased v0.1.2 candidate; see the [approved contract](../architecture/remote-workspace-v013.md) and [development log](../logs/DEVLOG.md).

- `X00-T17` Automated-only test sequence and release policy — done; seven areas reuse 31 CTest targets with deduplication, guarded test-only builds, bounded serial runs and JSON/JUnit/case receipts. Twelve manual/native cases are excluded at CTest registration; three desktop probe targets remain developer-invoked only. The final affected 24-suite build/run passed with zero failures/skips. Self-regression checks selection, manual exclusions and result/exit classification. No app publication, role change or peer action. See [test matrix](../testing/test-matrix.md).
- `X00-T18` Optional future native-fixture automation — planned, non-blocking; a developer may later make additional clipboard/input scenarios unattended and then register them as automatic cases. Current physical-key/application/foreground evaluations are the developer's choice, not mandatory work, automatic skips or release conditions.
- `X00-T19` Publish actual UPnP external UDP endpoint to ICE — done; successful mappings produce an additional standard srflx candidate for the matching host socket in trickle/SDP, preserving native routes and existing wire compatibility. Debug main build and five focused candidate/SDP/native-channel cases passed (one Winsock fixture initialization correction and single-case recheck). No full matrix or release-version qualification was repeated. Authorized local Host delivery and live connection evidence remain in local run reports, distinct from peer-reported port-probe success.
- `X00-T20` Long-session media slowdown — done locally for the reproduced sender-collapse path; the final controller no longer treats one low delivery batch or an unconfirmed probe as a new baseline, requires repeated pressure evidence and never learns token pacing as ordinary FIFO service. A 45-second trace delivered 1256 contiguous frames at 27.918 FPS with zero gaps above 300 ms; a 300.511-second same-instance soak sustained 27.673 FPS, with every ten-second window at 26.2–28.4 FPS, pacing 6.792–11.826 Mbps, one-frame/33.333 ms queue target and zero loss/deadline/dependency/encode/send failures. This exceeds the original 19.384 FPS / 15 large-gap baseline and eliminates the intermediate 162 kbps / 1.3 FPS / 806 ms queue collapse. Focused test and each Debug candidate build passed once; complete installed and rollback manifests verify. Commits `cf9e368` and `4c1bbb2` are local, not pushed. See [diagnostic handoff](../testing/desktop-latency-quality-investigation-20260915.md).

## Release gate

Controller restart follow-up (2026-09-20) — done: the user-authorized switch now runs the published `f5c9bfe` Debug GUI/runtime with unchanged connection settings and local UDP 55000. DHT/ICE and streaming recovered; five-second decode/presentation deltas were +25/+24 with zero synthetic/decode/presentation failures. Two remote srflx candidates were observed, without claiming a particular selected port. This supersedes the pre-restart runtime boundary in the publication receipt below.

Controller-side X00-T19 follow-up (2026-09-20) — done: synchronized `f5c9bfe` and preserved local work. The received remote candidates used UDP 55000; an authenticated diagnostic probe to the reported mapped UDP 55001 returned success, without switching the GUI connection. Standard Debug build/publication, five focused candidate/SDP/native mapped-port cases (zero skips), 20 native-file hash comparisons and the published command-entry smoke passed. The previous complete Debug directory was retained; active-runtime switching and physical post-fix desktop acceptance were not performed. See the [runbook evidence boundary](../testing/cross-lan-dual-machine-integration-playbook.md#8-失败分类).

The Developer Preview requires successful builds, unattended CTest checks, a clean portable archive and automated local endpoint evidence for affected cross-component behavior. Reuse evidence only when relevant source, dependencies, configuration and artifact identity still match; do not repeat unchanged cases for each documentation or packaging edit. The product performance baseline remains visible but does not block feature delivery solely because a machine-specific target is missed.

Supported-version directions, rolling upgrade and reconnect remain automatic checks using local endpoint bundles. Release requires only unattended checks. No manual physical cross-LAN run, actual application paste, physical keyboard, UAC/account action or visual confirmation is required to release a version. These evaluations may be chosen by the developer and do not appear as pending automatic acceptance. Report only the coverage actually executed; physical deployment remains separate.
