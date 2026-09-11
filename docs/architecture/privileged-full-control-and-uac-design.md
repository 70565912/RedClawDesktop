# Privileged Full-Control and Remote UAC Confirmation Design (Windows)

Date: 2026-03-20
Status: design-ready
Scope: Desktop controller and Windows host service path only.

## Goals
- Enforce strict verification before granting full-control capability.
- After verification, allow remote operator to view and confirm UAC consent prompts.
- Preserve explicit policy boundaries and complete auditability for privileged actions.

## Non-Goals
- Bypass Windows security controls.
- Grant full-control rights to mobile companion endpoints in this phase.
- Provide unrestricted shell execution by default.

## Capability Model
- `view-only`: video only, no input.
- `standard-control`: keyboard/mouse on user desktop only.
- `full-control`: includes secure desktop path and UAC consent interaction when policy allows.

Full-control is disabled by default and requires step-up verification.

## Trust and Verification Pipeline
1. Session bootstrap verification
- Validate session bootstrap secret and handshake authenticity.
- Validate replay and freshness windows.

2. Device trust validation
- Match controller device fingerprint against policy.
- Apply optional allowlist and risk profile.

3. Operator authentication
- Require operator identity proof bound to session.
- Require second factor for full-control requests.

4. Explicit full-control confirmation
- Show high-risk confirmation prompt on controller.
- Issue short-lived elevation grant token (single session, single scope).

## Process Architecture
1. `RedClawHostService` (LocalSystem, session 0)
- Owns privileged policy enforcement.
- Brokers secure desktop capture/input permissions.
- Records immutable privileged audit events.

2. User session helper
- Handles normal desktop capture/input UX.
- Cannot self-upgrade capability to full-control.

3. Privileged control broker
- Local IPC service owned by host service.
- Accepts signed `requestPrivilegedControl` calls.
- Validates grant token, policy, and timeout.

## UAC Confirmation Flow
1. Remote operator requests full-control.
2. Host service validates step-up requirements and policy.
3. Host service enables secure-desktop compatible capture/input channel.
4. Controller receives secure desktop stream and UAC dialog context.
5. Operator performs explicit allow/deny action.
6. Host service emits audit record with action result.
7. Capability returns to standard-control on timeout, disconnect, or policy trigger.

## Security Controls
- Step-up verification TTL: short-lived (for example 60 to 180 seconds).
- One grant token cannot be reused across sessions.
- Max failed privileged attempts trigger lockout and alert.
- High-risk actions require reason code in audit payload.
- All privileged traffic stays inside encrypted control channel.

## Audit Schema (minimum)
- `event_id`
- `timestamp_utc`
- `session_id`
- `operator_id`
- `controller_fingerprint`
- `target_host_id`
- `capability_before`
- `capability_after`
- `uac_action` (`approve` | `deny` | `timeout` | `blocked`)
- `policy_decision`
- `reason_code`

## Failure and Recovery
- Secure-desktop channel unavailable: deny privileged activation and keep standard session alive.
- Token expired mid-flow: downgrade to standard-control and require re-verification.
- Service-helper IPC fault: fail closed for privileged path, preserve transport if safe.

## Module Mapping
- M03: handshake replay/freshness and identity proof.
- M06: secure-desktop input policy gate (`M06-T04`).
- M07: privileged broker and UAC confirmation orchestration (`M07-T04`).
- M10: privileged audit emission and support bundle fields.

## Acceptance Gates
- Authorized operator can complete UAC confirmation remotely under full-control policy.
- Unauthorized or unverified operator cannot inject UAC consent input.
- All privileged actions produce complete audit records.
- No capability escalation without explicit verified grant.
