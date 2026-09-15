# Project State

Updated: 2026-09-16

## Current release

RedClawDesktop `v0.1.1` is the current Windows x64 Developer Preview. Its scope is the GUI Host/Controller workflow, public-DHT rendezvous, ICE connectivity, real desktop capture and codec path, D3D11 presentation, explicitly authorized input, and the optional remote Agent channel. This patch release improves local Codex CLI discovery and keeps Agent account feedback reachable in the settings dialog.

The portable ZIP is the only binary distribution for this release. The service MSI remains an unsigned development scaffold and is excluded from the release.

## Verified local baseline

The `v0.1.2` candidate is under qualification for M01-T03. Serial Debug/Release builds and an earlier 102-target CTest sweep passed. After the final Agent approval correction, the full sweep had one UI heartbeat failure and a DDA environment skip; the focused UI rerun passed while DDA remained denied. Do not report the final code as an unconditionally passing full sweep. Focused recovery/cursor checks and isolated rollback/Agent-job-independent upgrade checks passed. Earlier real local DDA, WGC and GDI probes succeeded; new/new, both directions against the pre-upgrade Debug bundle, and both directions against the published v0.1.0 and v0.1.1 Release bundles displayed real video with zero synthetic and decode/presentation failures. A DHT mixed-version Host restart recovered. An isolated non-input desktop reproduced and visually verified the first-frame capture-pause UI repair. Candidate ZIP hashing and extracted command-entry smoke passed. Physical cursor/recovery, rolling deployment, packaged GUI and publication remain pending. The current published release remains `v0.1.1`.

The controlled local native-size comparison measured 17.71% / 17.53% total CPU and 480.9 / 490.0 MiB working set for the old/new four-process GUI/runtime pair. This is one matched-scene observation, not a performance-improvement claim. Native QSV frame-pool recreation could not run on the local driver: D3D11-to-QSV device derivation failed, and the runtime retains its existing CPU-upload stability policy. Real Provider readiness and diagnostic paint replay were opt-in skips inside the otherwise passing full CTest run.

- Debug and Release builds use `build.ps1` so the matching app-local runtime is staged under `release/<Configuration>`.
- The local two-GUI path has exercised real capture, encode, transport, decode, presentation, authorized input, and Control/Media/Agent channel activity.
- The default ICE UDP port is 55000. GUI, CLI, runtime profiles, and integration scripts propagate the same setting. Local two-process tests use Controller 55001.
- UPnP targets the configured ICE UDP port. DHT keeps its independent listening port and does not request a router mapping.
- GitHub Release publication triggers a checksum, archive-content, and packaged command-entry smoke on a fresh hosted Windows runner. Real capture, GPU presentation, router mapping, cross-site traversal, input, and Provider login remain local or physical acceptance work.
- Startup reserves the configured ICE port so an occupied port fails early instead of silently changing the runtime contract.

## Verified cross-LAN checkpoint

One physical Controller-to-Host Debug run connected through public DHT and a direct NAT path. ICE reached connected, the negotiated channels opened, and real H.264 receive, hardware decode, and GUI presentation counters advanced without decode or presentation failures. The detailed machine-specific evidence remains in the private archive. This checkpoint did not cover remote input, a real Agent Provider, mixed-version interoperability, TURN fallback, or Release qualification.

## Acceptance policy

Functional correctness, channel continuity, real media progression, input authorization, package integrity, and absence of decode/presentation failures remain release requirements. Strict performance targets are engineering observations because results vary by hardware, driver, resolution, and network. A measured code regression still requires investigation, but an unmet aspirational threshold alone does not block feature development or this Developer Preview.

## Open work

1. Expand physical cross-LAN coverage across additional NAT types and validate configured TURN fallback.
2. Harden, sign, and validate the unattended service installer before publishing an MSI.
3. Continue GUI scheduling and large Agent-output optimization against the product performance baseline.
4. Resume Android-first portable client work after the Windows connection flow is stable.

## Operator boundary

For cross-machine Agent collaboration, use the [remote Agent result contract](../../.agents/skills/remote-agent-result-contract/SKILL.md). The Controller specifies the authorized objective and acceptance results; the endpoint Agent owns local permission decisions, method selection, execution and verification, returning redacted outcomes only. Keep raw endpoint context local and preserve genuine safety/authorization boundaries; an unrelated stage failure must not block independently authorized work.

The repository is not a communication or signaling exchange. Cross-machine coordination uses the established Control/Agent channels when connected and explicit operator actions during recovery. Runtime signaling, encrypted blobs, local paths, credentials, and evidence archives remain outside Git.

## Resume point

Current execution covers the authorized v0.1.2 capture/cursor delivery and the separately scoped [v0.1.3 remote workspace](../architecture/remote-workspace-v013.md). The formal local runtime remains temporarily Host; the requested physical final arrangement is local Controller and peer Host. The peer has not restored its desktop/Agent channel. Resume the existing peer Agent task when it reconnects, without another executor or duplicate maintenance task. Local v0.1.3 file transfer, shared mutation gates, terminal, navigation/Agent layout and received-copy actions have focused tests and actual online two-endpoint window evidence. Independent full-directory upgrades and an actual terminal-issued Host restart completed, with a final receipt, fresh Shell and advancing real media. Final explicitly rebuilt full CTest completed with 112 passed, one DDA permission skip and zero failures; the earlier ICE expiry assertion failure passed alone and in the final full run. Final serial Debug/Release builds, five opt-in native terminal cases and all four mixed-version media combinations with published v0.1.0/v0.1.1 passed. Actual Controller runtime reconnect retained the Shell variable; the local Release ZIP passed hash, extracted command startup and missing-terminal rejection checks. Next complete actual application Ctrl+V, matched concurrent-load measurement, final package checks and physical rolling-upgrade acceptance. Record local fixtures separately from physical results. X00-T16 retains the detailed qualification status; no public v0.1.2 release or physical peer upgrade is claimed.

The operator has authorized documenting and pushing the diagnostic/synchronization changes, building the current candidate and restarting this machine as Host for access from the other computer. `X00-T14` is offline-validated; deployment and Host readiness must be reported separately from physical acceptance. Preserve the journal, approvals and a rollback runtime. On the next connection, follow the [latency and static-quality investigation handoff](../testing/desktop-latency-quality-investigation-20260915.md): collect a healthy baseline immediately, preserve recurrence evidence before recovery, and distinguish media estimates, real queueing, input delivery and image quality. Peer authorization remains endpoint-owned; direct Agent acceptance requires real synchronization, not queued/dispatched status. See [kanban](MODULE_KANBAN.md) and [development log](../logs/DEVLOG.md).

After `v0.1.1`, continue the cross-LAN playbook with independently recorded versions, negotiated capabilities, artifact hashes, Host ICE UDP 55000, and runtime evidence. Supported versions must interoperate through their common capability set. Treat a connected state as preliminary until real capture, receive, decode, and presentation counters advance.
