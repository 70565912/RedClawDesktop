# Solution Architecture (Serverless-First P2P Remote Desktop)

## Product Goal
A desktop remote control product that:
- Uses offline signaling exchange as the default bootstrap path.
- Requires no custom business backend to operate.
- Supports unattended access even before user login (where OS permits).
- Enforces strong remote identity verification and explicit session confirmation before granting full control capability.
- After successful policy verification, supports full-control sessions including secure-desktop UAC consent view and remote confirmation path.
- Prioritizes direct P2P links and falls back to TURN relay when required.
- Enables developer-direct interaction with AI coding tools on remote dev machines (agent CLI/Copilot/etc.) and returns raw execution results.
- Provides a portable mobile/tablet companion client for fragmented on-the-go development workflows.
- Avoids full-chain AI proxy interpretation that burns token without adding developer value.
- Provides a no-project-server mode based on DHT rendezvous, local helpers, IPv6, automatic port mapping, and ICE connectivity checks.

## Interaction Philosophy
- Human-in-the-loop by default: developers issue instructions and read results themselves.
- AI is execution assistant on remote machine, not a mandatory middleman relay layer.
- Transport should faithfully carry commands/results/logs, not reinterpret them unless explicitly requested.

## Hard Constraints
- Pure P2P without any reachable rendezvous or relay path cannot guarantee success under all real NAT conditions.
- The preferred no-project-server path uses public DHT bootstrap nodes and public/private STUN where available; TURN is optional and user-provided rather than a default product dependency.
- End-to-end security must hold even when relayed.
- Desktop UI is a mandatory product capability for remote-desktop usage; CLI-only runtime is not a releasable endpoint.
- Service-mode deployment requires installer-based delivery (install/upgrade/uninstall + service registration lifecycle) rather than manual binary copy.

## Layered Architecture
1. UI Layer
- Session creation/join, QR scan, permissions, diagnostics.
- Developer command workspace for remote AI tool invocation and result view.
- Portable mode views for phone/tablet: quick status cards, recent build/test timeline, read-only code/log snapshots, and guarded command presets.

2. Session Orchestration Layer
- Session lifecycle state machine.
- Policy and capability negotiation.
- Task dispatch context for developer-issued remote AI commands.

3. Media and Input Layer
- Screen capture, encode/decode, render.
- Input capture/injection.
- Clipboard and file transfer channels.
- Optional terminal/command output streaming optimized for code-review readability.

4. Secure Transport Layer
- DTLS/SRTP for media, secure data channels for control/file.
- Congestion and bitrate adaptation.

5. Connectivity Layer
- ICE candidate gathering and checks.
- STUN discovery and TURN fallback.

## Signaling Modes (Priority Order)
1. Offline signaling (MVP default)
- Offer/answer and trickle candidates exchanged via encrypted text blob or QR.

2. DHT-backed online rendezvous (current serverless target)
- Decentralized key-based discovery for encrypted offer/answer/candidate exchange.
- Uses public DHT bootstrap only to join the network; no RedClaw-owned public server is required.
- Must pair with ICE, STUN, IPv6, UPnP/PCP/NAT-PMP, and synchronized hole punching for the actual connection.
- Detailed design: `docs/architecture/serverless-dht-rendezvous-and-local-helper-design.md`.

3. Local helper (serverless support path)
- Optional always-on helper on a client, LAN computer, or supported NAS runtime.
- Can maintain DHT state, automatic port mappings, local encrypted records, and diagnostics.
- Does not guarantee cross-Internet relay unless the helper is itself reachable through IPv6, explicit port mapping, vendor tunnel, or another user-controlled path.

4. Local rendezvous service scaffold (development diagnostics only)
- Retained only for loopback/private protocol tests.
- It is not a product deployment path and must not make RedClawDesktop depend on a RedClaw-owned or user-self-hosted public signaling service.

The DHT product path uses an acknowledged, Host-generation-owned protocol so delayed records cannot mix independent ICE attempts. See `docs/architecture/reliable-fast-connection-negotiation-v2.md`.

## Protocol Compatibility Contract

- Every persisted or cross-machine protocol is explicitly versioned and capability-negotiated, including DHT signaling, session, media, control, Agent, and Debug Bridge traffic.
- All still-supported released versions must interoperate in both directions through their common capability set. Matching Git commits, product versions, or executable hashes are evidence fields, not connection requirements or acceptance gates.
- Protocol evolution is additive by default: receivers tolerate unknown optional fields, and senders use only negotiated capabilities. A mandatory incompatible change introduces a new schema and migration path; it must fail with an explicit `protocol_version_incompatible` diagnostic rather than appearing as a missing peer, malformed record, or indefinite wait.
- Every protocol change requires mixed-version, rolling-upgrade, and reconnect coverage between the newest build and each still-supported protocol version. Same-version tests remain useful baselines but cannot establish compatibility acceptance.

## Security Model
- Session bootstrap secret (PSK/passphrase) required.
- Ephemeral key agreement per session.
- Forward-secure session keys and replay protection.
- No plaintext media/control payload on any transport path.
- Full-control capability (including UAC consent interaction) is disabled by default and only granted after multi-factor policy checks.
- Every high-risk privileged action requires auditable confirmation records with actor identity and timestamp.

## Unattended + Pre-Login Access
### Windows baseline approach
- Install host service running as LocalSystem.
- Service starts at boot automatically before user login.
- Host process can accept incoming session setup and expose login-screen capture/input path according to desktop/session permissions.
- Secure-desktop (UAC consent) capture and confirmation interaction are part of the Windows full-control path when policy allows.
- Separate user-mode helper process after login for richer desktop integration.

## Privileged Control Policy
- Connection handshake must complete device trust validation, operator authentication, and explicit full-control confirmation.
- Full-control session grants complete remote operation rights for the authorized scope, including UAC consent-screen interaction.
- Policy profiles should support deny/allow by device fingerprint, user role, time window, and risk level.
- Mobile companion remains capability-limited in this phase and does not inherit desktop full-control rights by default.

## DirectX Game Window Compatibility
- Windows capture pipeline must support common DirectX game window presentation paths (bordered/windowed and borderless-fullscreen where allowed by OS/driver).
- Capture subsystem should expose capture-mode fallback strategy (Desktop Duplication -> Windows Graphics Capture) with telemetry on selected path.
- Input/render path should preserve low-latency behavior under high frame-rate game workloads within configured bitrate/FPS budgets.

### Platform notes
- macOS and Linux need separate daemon/privilege models and may have stricter pre-login limitations.
- Define feature parity matrix by platform early.

## Recommended Stack for MVP
- Core language: Rust or C++ (performance + native integration).
- Connectivity/transport: WebRTC native stack or libdatachannel.
- Media pipeline: platform capture APIs + hardware codec wrappers.
- UI: Qt (native desktop consistency).

## Stack Optimization Decision Update (2026-04-02)

### NAT Traversal and P2P Transport
- Keep WebRTC/libdatachannel as transport baseline.
- Rationale:
	- Existing implementation and tests already validate this path.
	- ICE/STUN/TURN + DTLS/SRTP behavior is production-proven for low-latency real-time transport.
- Decision:
	- Do not replace transport layer with libp2p.
	- libp2p may be used only for optional discovery/signaling experiments, not as media/control transport baseline.

### Signaling Strategy (Hybrid)
- Keep offline signaling as mandatory diagnostic and recovery capability.
- Promote DHT-backed online rendezvous to the preferred no-project-server target.
- Add an optional local helper as a serverless support path.
- Keep the temporary signaling service only as a local development scaffold; keep TURN as an optional user-provided compatibility path.
- Target model: `DHT rendezvous default + offline recovery + optional local helper + optional user-provided relay`.

### Password and Cryptography
- Keep OpenSSL as cryptography baseline in current implementation.
- Current baseline uses PBKDF2 + AES-256-GCM for encrypted offer blobs.
- Roadmap hardening:
	- Upgrade password-based KDF toward Argon2id policy for offline blob protection.
	- Keep protocol-versioned migration strategy for backward compatibility.
	- Maintain replay-protection and fingerprint-trust model as mandatory gate before privileged control.

### Video Capture
- Keep Windows native capture path baseline: Desktop Duplication with Windows Graphics Capture fallback.
- Do not switch capture baseline to FFmpeg libavdevice for Windows host runtime.
- Reason:
	- Native capture paths provide lower-latency and tighter OS integration for remote desktop workloads.

### Video Encoding
- Recommended execution baseline for implementation stage:
	- FFmpeg libavcodec integration as codec abstraction layer.
	- Hardware encode priority: NVENC and QuickSync (with software fallback).
	- Preserve low-latency profile contract already defined in capture module.

## Implementation Priority Plan (Stack Optimization)
1. P0 (immediate)
- Runtime ICE profile configuration and diagnostics-first operation (candidate-type visibility, signaling timeout, fail-fast).
- Runtime role-path reliability and cross-network troubleshooting instrumentation.

2. P1 (next)
- Land real encoding backend integration (libavcodec + NVENC/QSV + software fallback).
- Add runtime capability negotiation for codec/profile fallback.

3. P2 (follow-up)
- Hybrid signaling optional temporary service path.
- DHT signaling experimental plugin track.
- KDF policy upgrade (PBKDF2 compatibility + Argon2id migration path).

## Portable Mobile/Tablet Companion (Phase Plan)
- Product role:
	- Companion endpoint, not full replacement for desktop controller in early phases.
	- Optimized for short sessions: check status, trigger safe commands, inspect logs/results, and perform quick remediation.
- Client technology (recommended):
	- Flutter for Android phone/tablet first implementation.
	- Native platform wrappers only where needed (push notifications, secure key storage, background policies).
- Decision record:
	- `docs/architecture/adr-mobile-client-tech-choice.md`
- Transport model:
	- Reuse existing secure session channels and signaling primitives from desktop stack.
	- Mobile connection profile favors low-bandwidth telemetry and text-first views, with optional low-fps preview stream.
- Security profile:
	- Device binding + short-lived session tokens.
	- Biometric unlock gate before sensitive actions.
	- Explicit high-risk action confirmation policy on mobile.
- UX boundaries (first portable release):
	- Include: diagnostics/status, command preset execution, artifact/log browsing, alert handling.
	- Exclude: controlled keyboard/mouse input and unrestricted shell control by default.

## Portable Platform Scope (Current)
- In implementation scope now:
	- Android phone/tablet companion client.
- Planning-only scope now (no implementation in current stage):
	- mac portable companion variant.
	- generic web mini access client.

## Toolchain Baseline (Windows First)
- IDE and compiler: Visual Studio 2022 + MSVC.
- Build system: CMake + Ninja.
- UI toolkit: Qt (`msvc2019_64` kits currently available on target machine).
- Note: when using `msvc2019_64` Qt kits, MSVC `v142` toolset is required in VS2022.
- Auxiliary scripting: Python for tooling and automation scripts.

## Delivery Requirements (Current Stage)
- Mandatory UI deliverable:
	- Desktop host/controller UI for session create/join, role selection, and runtime connection/error visibility.
- Mandatory installer deliverable:
	- Windows installer package that provisions host service deployment lifecycle (install/start/upgrade/uninstall) with deterministic rollback/error reporting.

## Non-Goals for MVP
- Full IDE-equivalent editing experience on mobile/tablet in MVP.
- Enterprise IAM/SSO.
- Multi-tenant cloud control plane.
- Autonomous AI-to-AI relay pipelines that hide raw execution details from developers.
