# Module Specifications and Test Samples

## M01 Connectivity Engine (ICE/STUN/TURN)
Scope:
- Candidate gathering, connectivity checks, nomination, keepalive.
- Direct-first path selection and relay fallback.

Interfaces:
- `startGathering(config) -> GatheringHandle`
- `applyRemoteCandidate(candidate)`
- `onConnectionStateChanged(callback)`

Test samples:
- Unit: parse and validate candidate strings.
- Unit: priority ordering logic for direct vs relay.
- Integration: two peers in same LAN establish direct path.
- Integration: simulated symmetric NAT forces relay fallback.

Acceptance:
- Connection success rate target met in test matrix.

## M02 Offline Signaling Exchange
Scope:
- Generate encrypted offer blob and QR.
- Parse remote answer blob and incremental candidates.

Interfaces:
- `createOfferBlob(sessionInitData, passphrase) -> blob`
- `consumeAnswerBlob(blob, passphrase) -> answer`

Test samples:
- Unit: blob schema encode/decode with versioning.
- Unit: decryption failure path on wrong passphrase.
- Integration: copy/paste full flow from offer to connected session.

Acceptance:
- End-to-end flow succeeds without any signaling server.

## M03 Secure Session and Identity
Scope:
- PSK-gated bootstrap, ephemeral key agreement, channel key rotation.

Interfaces:
- `beginHandshake(peerInfo, pskHint)`
- `verifyPeerFingerprint(fingerprint)`

Test samples:
- Unit: reject replayed handshake message.
- Unit: key derivation vectors match expected values.
- Integration: MITM simulation fails authentication.

Acceptance:
- No plaintext application payload in transit.

## M04 Screen Capture and Encode
Scope:
- Capture desktop frames and encode with low-latency presets.
- Support rendering-path capture for common DirectX game windows on Windows.

Interfaces:
- `startCapture(displayId, captureConfig)`
- `onEncodedFrame(callback)`

Test samples:
- Unit: frame pacing and backpressure handling.
- Integration: sustained capture for 30 minutes without leak.
- Integration: adaptive bitrate reacts to constrained bandwidth.
- Integration: DirectX game window capture (windowed/borderless) with stable frame output.

Acceptance:
- Target fps and latency reached on baseline hardware.
- DirectX game window image is visible remotely without persistent black-frame failure.

## M05 Decode and Renderer
Scope:
- Decode frame stream and render with GPU path.

Interfaces:
- `onEncodedFrame(frame)`
- `setViewport(mode)`

Test samples:
- Unit: decode reorder and timestamp handling.
- Integration: resolution switch without crash or freeze.

Acceptance:
- No sustained frame drops over threshold in baseline test.

## M06 Input Injection and Permission Control
Scope:
- Keyboard/mouse injection with policy checks.
- Include policy-gated secure-desktop (UAC consent) input path for full-control desktop sessions.

Interfaces:
- `injectInput(event)`
- `setPermission(policy)`
- `setPrivilegedControlMode(mode)`

Test samples:
- Unit: blocked event when policy denies control.
- Integration: hotkey combinations map correctly.
- Integration: secure-desktop input allowed only when full-control policy is active.

Acceptance:
- Permission toggles take effect immediately and safely.
- UAC consent interaction is blocked by default and enabled only after strict verification and policy grant.

## M07 Unattended Service and Boot-Time Host
Scope:
- Install host as startup service and support pre-login connections.
- Broker privileged remote actions and UAC consent confirmation with auditable records.

Interfaces:
- `installService(config)`
- `startHostService()`
- `handoffToUserSession(sessionId)`
- `requestPrivilegedControl(sessionId, authProof)`
- `confirmUacConsent(sessionId, consentAction)`

Test samples:
- Unit: service config parser and policy validation.
- Integration: reboot machine, service auto-start confirmed.
- Integration: connect before login screen and maintain session after user login handoff.
- Integration: full-control verified session can complete UAC consent flow remotely.

Acceptance:
- Remote connection works without interactive login when policy allows.
- Privileged confirmations are security-gated and fully auditable.

## M08 Clipboard and File Transfer
Scope:
- Clipboard text/image sync and resumable file transfer.

Interfaces:
- `syncClipboard(payload)`
- `sendFile(stream, resumeToken)`

Test samples:
- Unit: chunk integrity and retransmission logic.
- Integration: interrupted transfer resumes from checkpoint.

Acceptance:
- Data integrity verified by end-to-end checksum.

## M09 Session Orchestration and State Machine
Scope:
- Deterministic session lifecycle and failure recovery.

Interfaces:
- `startSession(request)`
- `terminateSession(reason)`
- `getSessionState()`

Test samples:
- Unit: state transition table validation.
- Integration: network flap recovery within timeout.

Acceptance:
- No illegal state transitions in stress tests.

## M10 Diagnostics, Metrics, and Logging
Scope:
- Structured logs, connection stats, and local troubleshooting bundle.

Interfaces:
- `emitMetric(name, value, tags)`
- `collectSupportBundle()`

Test samples:
- Unit: redact sensitive fields from logs.
- Integration: support bundle generation under active session.

Acceptance:
- Required debug fields available without leaking secrets.

## M11 Mobile/Tablet Portable Companion
Scope:
- Android phone+tablet companion endpoint for fragmented remote development workflows.
- Session status view, safe command presets, notifications, and artifact/log browsing.
- Policy-gated sensitive action model for mobile.
- No controlled keyboard/mouse input in current mobile scope.
- mac portable variant and generic web mini access remain planning-only in this stage.

Interfaces:
- `bindDevice(deviceInfo, authProof)`
- `openPortableSession(sessionToken)`
- `runCommandPreset(presetId, args)`
- `fetchRecentArtifacts(sessionId, filter)`

Test samples:
- Unit: command scope policy enforcement and deny-path coverage.
- Integration: mobile device binds, resumes session, and fetches latest build/test outputs.
- Integration: biometric/approval gate required for high-risk command presets.

Acceptance:
- Developers can reliably perform short remote maintenance and verification tasks from Android phone/tablet without exposing unrestricted control by default.

## Cross-Module Interface Contracts (Implementation -> Integration Gate)

This section records the concrete cross-module contracts currently used on the critical path.

### Connectivity and Signaling
- M01 -> M02
	- Contract: session offer/answer/candidate exchange consumes M01 connectivity callbacks.
	- Surface: `startGathering(config)`, `applyRemoteCandidate(candidate)`, `onConnectionStateChanged(callback)`.
- M02 -> M03
	- Contract: decrypted signaling payloads are forwarded into secure handshake ingress.
	- Surface: `createOfferBlob(sessionInitData, passphrase)`, `consumeAnswerBlob(blob, passphrase)`, `beginHandshake(peerInfo, pskHint)`.

### Session Security and Runtime Bootstrap
- M03 -> M07
	- Contract: trust-store and fingerprint validation policy are resolved at service bootstrap and enforced before privileged operations.
	- Surface: `verifyPeerFingerprint(fingerprint)`, trust-store env contract (`REDCLAW_TRUST_STORE_PATH`, `REDCLAW_TRUST_STORE_FALLBACK`).

### Capture, Render, and Session Lifecycle
- M04 -> M05
	- Contract: encoded frame stream generated by capture/encoder path is consumed by decode/render queue.
	- Surface: `onEncodedFrame(callback)` (M04 producer), `onEncodedFrame(frame)` (M05 consumer).
- M05 -> M09
	- Contract: renderer lifecycle and resolution/state changes follow deterministic session-state transitions.
	- Surface: `setViewport(mode)`, `startSession(request)`, `terminateSession(reason)`, `getSessionState()`.

### Privileged Input and UAC Flow
- M07 -> M06
	- Contract: privileged-control and UAC consent runtime events gate secure-desktop input routing.
	- Surface: `requestPrivilegedControl(sessionId, authProof)`, `confirmUacConsent(sessionId, consentAction)`, `setPrivilegedControlMode(mode)`.
- M06 -> M09
	- Contract: input policy/runtime controller follows session orchestrator capability changes and fail-close events.
	- Surface: `injectInput(event)`, `setPermission(policy)`, session capability/state transitions.

### Observability and Audit
- M06/M07 -> M10
	- Contract: privileged decisions and deny-path outcomes emit structured logs and diagnostics-safe fields.
	- Surface: `emitMetric(name, value, tags)`, `collectSupportBundle()` plus structured audit log formatting path.

### Integration Contract Verification Map
- Protocol/schema and consent ingress: `redclaw_protocol_schema_v1_tests`, `redclaw_m07_session_uac_consent_ingress_router_integration_tests`.
- Privileged/UAC runtime: `redclaw_m07_uac_prompt_runtime_coordinator_integration_tests`, `redclaw_m07_uac_consent_signal_producer_integration_tests`.
- Input policy/runtime routing: `redclaw_input_policy_gate_tests`, `redclaw_privileged_input_gate_integration_tests`, `redclaw_privileged_runtime_input_routing_integration_tests`.
- Service lifecycle bridge: `redclaw_service_windows_service_lifecycle_wrapper_tests`, `redclaw_host_service_runtime_lifecycle_bridge_tests`.
