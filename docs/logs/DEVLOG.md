# Development Log

This public log records release-level changes. Machine-specific paths, addresses, runtime signaling, credentials, and raw evidence are intentionally excluded.

## 2026-09-15 — X00-T14 Agent synchronization rejection handling

- Follow-up authorization now covers committing/pushing the scoped changes, rebuilding and restarting this machine as Host. Added the [diagnostic handoff](../testing/desktop-latency-quality-investigation-20260915.md) with established sender-throttling evidence, the unproven queue-estimator trigger, a separate static-quality investigation, immediate healthy baselines and recurrence-before-recovery evidence. Corrected the current task board's superseded missing-key blocker from the later peer result. Deployment/standby and physical incident acceptance remain separate gates.

- Read-only local evidence identified repeated remote `request_rejected / not authorized` responses, not successful task snapshots. The Controller had persisted the executor's placeholder paused state and kept retrying synchronization; a pending-sync UI badge also hid the authorization status. No remote permission was changed or bypassed.
- Request-level rejections no longer overwrite durable task state, approvals or ACKs. Complete authorization-denial capabilities stop automatic sync retries without clearing pending work. Authorization recovery and a new epoch require fresh synchronization of nonterminal tasks; in-flight snapshots during denial cannot waive that recovery gate. Validate epoch envelopes before changing state and preserve replay-guard consistency when persistence fails. Offline/unauthorized UI states take precedence over syncing.
- Four focused regressions were observed failing before their fixes. Final serial `build.ps1 -Configuration Debug -SkipConfigure -NoPublish -Target redclaw_desktop` and five focused Agent/GUI CTests passed: 22 coordination cases, 7 peer-session cases, 18 broker cases, 5 local-control cases and 94 UI cases passed; the existing opt-in diagnostic-paint replay case was skipped. Static peer review covered authorization recovery and epoch failure boundaries.
- No publication, replacement, restart, GitHub operation, remote command, authorization-record reset or live-journal repair occurred. The running executable and GUI/runtime identities remained unchanged. The candidate includes the pre-existing worktree changes. Physical acceptance still requires separately authorized local deployment and endpoint-owned authorization; this does not complete the picture-quality incident. A general request rejection can still leave a locally queued request pending in the UI until explicit synchronization; its diagnostic is retained in the normal log, not promoted to a provider task result.

## 2026-09-15 — X00-T13 remote Agent result contract

- Added an instruction-only repository skill and mandatory AGENTS routing: the Controller provides authorized goals and result criteria, while the endpoint Agent chooses methods, handles local permission decisions and executes checks locally. Only necessary redacted outcomes cross the collaboration boundary, not raw process, account, permission or command details.
- Separate archive extraction/validation from live deployment dependencies; a blocked stage does not invalidate independently completed work. Preserve local approvals, secret handoff, single-executor ownership and task-versus-incident completion distinctions. Delegation never bypasses an explicit denial or grants new authority.
- Skill format and navigation checks passed. An independent six-scenario paper exercise covered missing keys, diagnostic-only authorization, raw-environment requests, unavailable tools during an authorized deployment, prohibited cross-agent delegation and monitor closeout; feedback clarified operation-specific denial scope and overall completion criteria. No product code, runtime settings, build, restart, remote execution or credential publication was performed. The skill defines the new collaboration policy; distribution and adoption on another machine are not implied by local creation.

## 2026-09-15 — M08-T03 Host media timestamp-pair diagnostics

- Interface scope: expose the estimator's latest applied send/receive timestamp pair, its feedback/packet/frame/revision identity, anchors and EWMA/minimum through a typed read-only diagnostic snapshot. Keep a single fixed-size sample under the existing estimator lock; format it only in the existing periodic Host log path. No payload, input content, credentials, new wire fields or congestion-policy changes.
- Seven new deterministic C++ tests cover fixed delay/offset, positive/negative clock-rate skew, actual delay steps, sample retention/reset/rejected feedback and single-line snapshot bounds. With no growing network delay, a +30 ppm receiver clock produces a 287 ms queue estimate after 160 virtual minutes and triggers severe congestion pressure; zero and -30 ppm produce zero queue. This characterizes the unchanged estimator, not measured endpoint skew or an incident fix.
- The serial offline Debug desktop build and all three focused CTests (stream adaptation, video frame transport, transport recovery) passed. The adaptation executable passed all 13 cases. The desktop build retains existing `getenv` deprecation warnings. No Release/full-suite or physical deployment validation is claimed.
- Later peer results reported successful candidate validation/restart and restored throughput, while the operator still reported static blur. The former missing-key blocker is superseded; queue-estimator root cause and physical quality acceptance remain open. See the current diagnostic handoff.

## 2026-09-14 — Agent conversation panel content-sized window

- Connected the virtualized conversation viewport's measured content height to its top-level window size hint. The Agent panel now grows as remote replies and expanded activity details are measured, coalesces streaming resize requests, and clamps the result to the active screen so long histories remain scrollable.
- Added UI regressions for window growth/shrink and expanded wrapped activity details. The shared Debug build and full UI connection-flow suite passed after using a consistent MSVC 14.44 compiler/header environment; seven related suites passed together. The latest Release publication and physical two-machine UI validation remain pending.

## 2026-09-14 — Opt-in input delivery diagnostics

- Extend the existing bounded input-stage recorder with category counts, explicit mapping/rejection stages, and native SendInput return/context observations; no key values or text are retained. Add a Debug-only diagnostic launch option, independent of synthetic Agent fixtures.
- Add a passive Host diagnostic window to distinguish OS native-message delivery from API insertion. It observes only its own window, does not install global hooks or grant input authority, and leaves normal Control/Agent wire formats unchanged.
- Debug desktop build with the online DHT backend and all seven focused suites passed: runtime options/receipts, input session, UI connection flow, Agent provider, coordination, broker and isolation. Physical deployment and target-window delivery evidence remain pending; no latency or UAC-control improvement is claimed.

## 2026-09-14 — Agent approval identity and bounded result replay

- Scoped durable approval lookup to task plus request ID. A provider counter reused after Host restart must not inherit another task's accepted/rejected approval; the existing journal schema is retained and replayed with the corrected key. Same-task duplicate decisions still fail closed; ordinary request uniqueness and cross-task replacement semantics are preserved.
- Changed transcript synchronization to cursor-based bounded batches. Transport queue pressure requests replay from the durable acknowledgment instead of reporting retained text as permanently lost; real provider/cache loss remains explicit. Added a long-history/small-queue regression.
- Applied bounded backpressure to dedicated provider pipe readers, waking blocked readers before shutdown. Preserved Codex command-output payloads, reported failed/interrupted turn completion correctly, and released failed process/turn ownership so an explicit retry can resume the registered thread.
- Retained the related Cursor first-approval retry repair: an unlaunched registered task starts only after fresh approval, while an already launched task must resume its registered chat.
- Agent v1 wire fields and security gates are unchanged. Debug build/publication and packaged startup passed, as did the six focused Agent/IPC test suites (including the 60-second isolation gate). Physical mixed-version runtime validation is tracked on `X00-T10`; remote UAC remains a field-evidence gate, not a conclusion from input counters.
- Physical upgraded-Controller/old-Host reconnection and task-scoped approval reuse succeeded. A read-only remote probe and Agent report readback found Default input desktop, no consent/LogonUI, and matching Host/probe sessions at sampling time; this does not reconstruct the earlier input failure. Host-side result-flood deployment/acceptance remains open.

## 2026-09-14 — Cursor and Codex conversation output parsing

- Parsed Cursor `stream-json` assistant content from nested `message.content`, enabled partial output, surfaced bounded tool-call summaries, and used the terminal `result` as a fallback when no assistant message was emitted. Duplicate buffered/final assistant output is filtered per turn.
- Extended Codex app-server handling beyond `params.delta` to nested `item` content and completion text, while keeping tool item details visible as bounded activity summaries. No Agent wire schema change was required.
- Added Provider regressions for nested assistant text, tool details, completion fallback, Cursor result de-duplication, and the existing approval/resume path. The full Agent Provider suite passed 18 tests with one opt-in real-provider readiness test skipped; all 17 conversation-panel tests passed.
- Release rebuild and packaged `--help` startup validation remain required after this parser change; physical two-machine Cursor/Codex validation remains open.

## 2026-09-14 — Agent message layout and Cursor approval retry

- Removed the blue background band around outgoing messages while retaining a single 1 px outline. Expanding or collapsing remote activity now invalidates that row's cached height, remeasures wrapped details, and repositions subsequent messages.
- Fixed follow-up submission after the first Cursor approval is rejected, expires, or is interrupted: register the task's model and workspace before approval, distinguish an unlaunched task from an existing chat, and require fresh approval before starting the new instruction. Started conversations still resume their recorded chat; unknown tasks and launched tasks without a chat ID remain rejected. No wire schema or capability changes were required.
- Reproduced both the stale expansion height and the exact missing-chat error with regression tests before fixing them. After the fix, 17 conversation-panel tests, 15 Provider tests, and five broker approval tests passed; the opt-in real Provider readiness test was skipped. Native Qt screenshot validation confirmed the single border and fully visible expanded Chinese/English details.
- Built and published the Release program through `build.ps1`, verified the published executable matches the build hash, and passed its `--help` startup check. The existing Debug instances were left running. Real two-machine Cursor conversation validation remains open.

## 2026-09-12 — GitHub unit baseline repair

- Kept backend-independent DHT listen-port selection in the base service library so the default unit build links its matching tests without installing the optional libtorrent and miniupnpc feature set.
- Added a vcpkg binary cache keyed by the runner, vcpkg revision, manifest, configuration, and overlays. Cold or changed dependency inputs still perform one complete build; matching later jobs can reuse compiled packages.
- Aligned the public vcpkg manifest version with the `0.1.1` CMake project version.
- Preserved the non-E2E Windows unit baseline and kept physical public-DHT, desktop, GPU, UPnP, and input acceptance outside the hosted runner.

## 2026-09-12 — v0.1.1 package verification

- Documented the boundary between GitHub-hosted checks and physical desktop acceptance, including the reason a fresh hosted runner installs its own vcpkg dependencies.
- Added a release-triggered Windows package smoke that downloads the final published ZIP, verifies its checksum and required files, and runs the packaged command entry without rebuilding the program.
- Kept real capture, GPU presentation, UPnP, cross-site ICE, focus-sensitive input, and real Provider login in the local or physical acceptance scope.
- Replaced the runtime-control writer's host-speed threshold with a deterministic pre-event-loop receipt check while retaining its measured delay as test telemetry.
- Fixed strict-mode CMake preset discovery so the release publisher can call the standard build entry point when visible presets omit the optional `hidden` field.

## 2026-09-12 — Local Agent login consistency

- Made Codex CLI discovery consistent for GUI launches that do not inherit Codex Desktop's temporary PATH: an explicitly available native CLI still wins, then the current user's Codex Desktop CLI is discovered before an older PowerShell launcher fallback.
- Kept Agent account status, cancellation, and command output inside the visible settings window, displayed the selected local CLI path when an account command starts, and compacted long Provider model lists.
- Verified the focused Provider and GUI tests, published Debug, and confirmed through the standard Controller launcher that Codex reports ready with the current CLI while the account feedback controls remain visible.

## 2026-09-11 — offsite desktop stream checkpoint

- Completed one physical cross-LAN Controller-to-Host run through public DHT and a direct NAT path. ICE connected, all four negotiated channels opened, and real H.264 capture, transport, hardware decode, and GUI presentation advanced without decode or present failures.
- This checkpoint covered the real video path. Remote input, a real Agent Provider, mixed-version peers, TURN fallback, and Release qualification remain separate acceptance work.

## 2026-09-11 — v0.1.0 public prerelease

- Published a clean public-repository baseline with Apache-2.0 licensing, bilingual product documentation, a sanitized GUI screenshot, release notes, and a Windows x64 portable-package publisher.
- Removed the repository-based communication exchange and its synchronization script. Debug Bridge retains local encrypted signaling files and explicit operator transfer; the runtime DHT local record store remains part of the rendezvous implementation.
- Changed the installer workflow to manual dispatch while the Windows service package remains unsigned and incomplete for public distribution.
- Fixed the Agent account Login action so its local command output, startup errors, non-zero exits, cancellation, timeout, and recovery guidance remain visible in the settings dialog.
- Defined functional package checks as release-blocking and kept hardware-sensitive performance targets as reported engineering observations.

## 2026-09-11 — ICE UDP port and UPnP ownership

- Added one `ice_udp_port` setting shared by CLI, runtime profiles, GUI settings, diagnostics, and PowerShell launch scripts. The product default is UDP 55000; local dual-process validation uses Controller 55001.
- Reserved the configured UDP port during startup, fixed the libdatachannel port range to that value, and made conflicts fail with an explicit error.
- Moved UPnP ownership from the DHT listener to the ICE session. Mapping failures stay visible without disabling STUN, TURN, or ordinary hole punching.
- Added focused tests for defaults, overrides, invalid values, propagation, fixed ICE ranges, port conflicts, and mapping state.

## Earlier development

The pre-public private repository contains the detailed implementation history and local acceptance evidence. The public repository starts from one sanitized root commit so internal coordination records and machine-specific evidence are not published.
