# Project State

Updated: 2026-09-20

## Current release

The operator selected current main as `v0.1.3` Windows x64 Developer Preview on 2026-09-20, combining the unpublished v0.1.2 capture/recovery candidate with file/folder transfer, on-demand clipboard, embedded PowerShell terminal and independent maintenance. Version metadata, bilingual README and [release notes](../releases/v0.1.3.md) are aligned. Publication is in progress; `v0.1.1` remains the latest public package until the new tag and assets are verified.

The portable ZIP is the only binary distribution for this release. The service MSI remains an unsigned development scaffold and is excluded from the release.

## Verified local baseline

For v0.1.3, serial Release build/publication and Debug NoPublish build succeeded. Runtime C++ is unchanged from main `88a3803`; this release task changes version metadata, documentation, automatic test registration/runner and packaging. Reuse the recorded functional, local endpoint and supported-version results instead of repeating them for publication. The reorganized 24-suite automatic selection passed with zero case skips; the runner's own regression and syntax checks passed.

A redundant new Release sweep was stopped on the operator's 2026-09-20 instruction not to repeat tests. Before cancellation it recorded four failed DHT listener cases reporting TCP bind error 10013; the sweep is incomplete, not a passing full run. Keep that log and distinguish it from the earlier passing coverage. Do not silently rerun it, suppress the failures or convert cancellation into acceptance. Final package integrity/startup and published-asset checks are separate from rerunning functional tests.

Historical v0.1.2 candidate evidence: serial Debug/Release builds and an earlier 102-target CTest sweep passed. After the Agent approval correction, a full sweep recorded one UI heartbeat failure and a DDA environment skip; the focused UI rerun passed while DDA remained denied. Focused recovery/cursor and isolated rollback/Agent-job-independent upgrade checks passed. Earlier local DDA/WGC/GDI probes, both pre-upgrade Debug directions, and both public v0.1.0/v0.1.1 directions displayed real video with zero synthetic and decode/presentation failures. A DHT mixed-version Host restart recovered; candidate ZIP hashing/extracted startup passed. Preserve the original artifact scope; physical cursor/recovery and actual peer rollout are optional developer evaluations, not release gates.

The controlled local native-size comparison measured 17.71% / 17.53% total CPU and 480.9 / 490.0 MiB working set for the old/new four-process GUI/runtime pair. This is one matched-scene observation, not a performance-improvement claim. Native QSV frame-pool recreation could not run on the local driver: D3D11-to-QSV device derivation failed, and the runtime retains its existing CPU-upload stability policy. Real Provider readiness and diagnostic paint replay were opt-in skips inside the otherwise passing full CTest run.

- Debug and Release builds use `build.ps1` so the matching app-local runtime is staged under `release/<Configuration>`.
- The local two-GUI path has exercised real capture, encode, transport, decode, presentation, authorized input, and Control/Media/Agent channel activity.
- The default ICE UDP port is 55000. GUI, CLI, runtime profiles, and integration scripts propagate the same setting. Local two-process tests use Controller 55001.
- UPnP targets the configured ICE UDP port. DHT keeps its independent listening port and does not request a router mapping.
- GitHub Release publication triggers a checksum, archive-content, and packaged command-entry smoke on a fresh hosted Windows runner. Human-dependent capture/GPU/topology/input/Provider evaluations remain optional developer work, not release gates.
- Startup reserves the configured ICE port so an occupied port fails early instead of silently changing the runtime contract.

## Verified cross-LAN checkpoint

The latest recorded physical Controller-to-Host Debug run (2026-09-20) connected through public DHT and a direct NAT path. Real H.264 receive, hardware decode and GUI presentation advanced without failures; one approved Cursor task completed. A temporary target visibly consumed a GUI click and a QA-protocol digit. This does not qualify physical-keyboard capture, TURN, a new Release build or actual workspace capability negotiation. Detailed machine-specific evidence remains outside Git.

## Acceptance policy

Functional correctness, channel continuity, real media progression, input authorization, package integrity, and absence of decode/presentation failures remain release requirements. Strict performance targets are engineering observations because results vary by hardware, driver, resolution, and network. A measured code regression still requires investigation, but an unmet aspirational threshold alone does not block feature development or this Developer Preview.

Use the [automated test matrix](../testing/test-matrix.md): unattended local tests own functional boundaries and local Host/Controller owns cross-component/supported-version compatibility. Only fully unattended cases enter automatic sequences and version-release requirements. Physical keys, actual application paste, real login/UAC/consent, special desktop/topology and visual assessment are developer-selected manual evaluations, not pending release gates. Do not request human completion after an automatic run, and do not claim unperformed physical coverage. Product security/compatibility rules remain unchanged.

## Open work

1. Expand physical cross-LAN coverage across additional NAT types and validate configured TURN fallback.
2. Harden, sign, and validate the unattended service installer before publishing an MSI.
3. Continue GUI scheduling and large Agent-output optimization against the product performance baseline.
4. Resume Android-first portable client work after the Windows connection flow is stable.

## Operator boundary

For cross-machine Agent collaboration, use the [remote Agent result contract](../../.agents/skills/remote-agent-result-contract/SKILL.md). The Controller specifies the authorized objective and acceptance results; the endpoint Agent owns local permission decisions, method selection, execution and verification, returning redacted outcomes only. Keep raw endpoint context local and preserve genuine safety/authorization boundaries; an unrelated stage failure must not block independently authorized work.

The repository is not a communication or signaling exchange. Cross-machine coordination uses the established Control/Agent channels when connected and explicit operator actions during recovery. Runtime signaling, encrypted blobs, local paths, credentials, and evidence archives remain outside Git.

## Resume point

Last observed runtime state (2026-09-20 input test): this machine was the `88a3803` Debug Controller, video connected and remote input paused after the temporary target disappeared. The pending request for the operator to type a digit was withdrawn. This is a recorded checkpoint, not a fresh liveness claim. No subsequent deployment, peer restart or role change is implied by the test-process reorganization.

The target's visible click counter and digit are valid application-consumption evidence. The peer's six received batches and twelve injected events support transport/injection; an additional peer application readback is not a required duplicate gate. Physical-keyboard Hook capture is not proven by software-injected keys. Any physical-key test is now optional developer evaluation, not an automatic follow-up or release condition; the safety filter stays intact.

Complete only v0.1.3 packaging, GitHub publication and published-asset verification; do not rerun completed functional tests. Existing transfer, terminal, upgrade/rollback, mixed-version and package evidence on `X00-T16` stays scoped to its artifacts. Do not schedule manual clipboard/key/peer checks to close these results. Developer-selected cross-LAN work is optional and physical deployment remains a separate authorized operation.

The operator's 2026-09-20 decision supersedes the former separate v0.1.2-first release boundary: ship the complete current main as v0.1.3. Automatic qualification and package/publication work remain on [kanban](MODULE_KANBAN.md); historical role/authorization context and evidence remain in the [development log](../logs/DEVLOG.md), not competing resume instructions. The [latency/quality incident](../testing/desktop-latency-quality-investigation-20260915.md) remains unresolved and requires matched evidence, not another full functional matrix. This release task does not authorize a physical peer upgrade or change the active Debug session.
