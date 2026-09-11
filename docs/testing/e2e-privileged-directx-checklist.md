# E2E Checklist: Full-Control UAC and DirectX Visibility

Date: 2026-03-20
Owner: M04/M06/M07/M10 joint validation

## Environment Preconditions
- Windows host service installed and running.
- Full-control policy enabled for test operator and test device.
- Audit sink enabled.
- Baseline DirectX test applications prepared (windowed and borderless).

## Scenario A: Full-Control Verification and UAC Approve
- Connect as trusted operator.
- Request full-control session.
- Complete step-up verification.
- Trigger a host action that raises UAC consent.
- Confirm UAC remotely.
- Verify expected host-side action succeeded.
- Verify audit record fields are complete and correct.

Pass criteria:
- UAC dialog is visible in remote stream.
- Remote confirm action is accepted once policy checks pass.
- Audit record includes operator, device, session, action, and result.

## Scenario B: Full-Control Verification and UAC Deny/Timeout
- Connect as trusted operator.
- Request full-control and complete verification.
- Trigger UAC prompt.
- Deny action or wait for timeout.
- Verify protected action does not execute.
- Verify audit result shows deny or timeout.

Pass criteria:
- No privileged action executes on deny or timeout.
- Session returns to expected capability state.

## Scenario C: Unauthorized UAC Input Block
- Connect without full-control policy or fail step-up verification.
- Trigger UAC prompt path.
- Attempt remote confirmation input.
- Verify input is blocked.
- Verify security alert or audit event exists.

Pass criteria:
- UAC input injection is denied.
- Deny reason is recorded.

## Scenario D: DirectX Game Window Visibility (Windowed)
- Launch DirectX test game in windowed mode.
- Start remote stream and observe capture stability for 10 minutes.
- Verify no persistent black frame condition.

Pass criteria:
- Continuous visible game content.
- Frame updates stay live without long freeze intervals.

## Scenario E: DirectX Game Window Visibility (Borderless)
- Switch game to borderless-fullscreen.
- Validate stream continuity and quality.
- Alt-tab in/out repeatedly.

Pass criteria:
- Game image remains visible after mode/focus transitions.
- Any backend switch is logged and recovery remains within policy threshold.

## Scenario F: Recovery and Telemetry Validation
- Simulate GPU reset or capture backend fault.
- Verify fallback and recovery path.
- Export diagnostics bundle.

Pass criteria:
- Recovery path executes without session crash.
- Diagnostics include capture backend and switch reasons.
