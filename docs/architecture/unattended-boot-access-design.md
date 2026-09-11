# Unattended Boot-Time Access Design (Windows First)

## Requirement
After software installation, remote host must be reachable after system boot without interactive user login, subject to security policy.

Additional mandatory requirement:
- Remote full-control sessions must perform strict verification before activation.
- Once verified and authorized, operator can view and confirm Windows UAC consent prompts remotely under policy.

## Design Overview
- Install a Windows Service (`RedClawHostService`) with startup type `Automatic`.
- Service runs under `LocalSystem` with minimal required privileges.
- Service starts core host components and listens for incoming session bootstrap.
- Before user login: provide secure-desktop compatible capture/input path where permitted.
- After user login: hand off or bridge to user-session helper process for richer desktop integration.
- Introduce a privileged control broker that gates full-control capability and records all privileged confirmations.

## Process Model
1. Service process (session 0)
- Responsibilities:
  - boot startup
  - connectivity and signaling bootstrap
  - policy enforcement
  - session broker and watchdog

2. User-session helper process
- Launched when interactive session appears.
- Responsibilities:
  - user desktop capture and input APIs
  - clipboard and file UX integrations

3. Broker channel
- Authenticated local IPC between service and helper.
- Used for capability sync and session handoff.

## Security Controls
- Unattended mode is disabled by default.
- Enable requires local admin action and strong device passphrase.
- Rate limiting and lockout for failed remote attempts.
- Mandatory audit trail for unattended sessions.
- Optional allowed-device fingerprint list.
- Full-control + UAC interaction requires elevated policy profile and explicit operator confirmation during session setup.
- Require step-up verification before enabling UAC consent interaction (for example, second factor or short-lived approval token).
- Persist immutable audit records for UAC confirmations (operator, device, target, timestamp, result).

## Failure Handling
- Helper crash: service attempts bounded restart and keeps transport alive.
- Session switch: state machine transitions with reconnect hints.
- No interactive session: continue in pre-login policy mode.

## Test Cases
1. Install and boot auto-start
- Verify service state after reboot before login.

2. Pre-login connect
- Verify controller can establish authorized session pre-login.

3. Login handoff
- Start session pre-login, then login locally, verify controlled handoff.

4. User logout fallback
- Active session survives or reconnects via defined policy after logout.

5. Security lockout
- Repeated auth failures trigger lockout and audit event.

6. Upgrade safety
- Service binary upgrade without breaking startup registration.

7. Remote UAC confirmation path
- After successful strict verification, operator can view UAC consent desktop and confirm action remotely.

8. UAC confirmation deny path
- Without full-control policy or failed step-up verification, UAC confirmation input is blocked and audited.

## Implementation Notes
- Keep privilege boundaries explicit.
- Avoid embedding UI logic inside service process.
- Ensure all sensitive material remains in memory only and is zeroized on teardown.
