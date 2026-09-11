# M07-T04 Interface Draft and State Machine (Windows)

Date: 2026-03-20
Status: implementation-ready draft
Owner: M07 (with M03/M06/M10 dependencies)

## Purpose
Define concrete service-side interfaces and deterministic state transitions for privileged full-control activation and remote UAC confirmation.

## IPC Boundary
- Producer: desktop controller session layer.
- Consumer: `RedClawHostService` privileged broker.
- Transport: authenticated local IPC channel between service and helper.

## Data Types
```cpp
namespace redclaw::service {

enum class CapabilityLevel {
    kViewOnly,
    kStandardControl,
    kFullControl,
};

enum class PrivilegedDecision {
    kAllow,
    kDeny,
    kTimeout,
    kBlocked,
};

enum class PrivilegedError {
    kNone,
    kInvalidSession,
    kPolicyDenied,
    kStepUpRequired,
    kTokenExpired,
    kReplayDetected,
    kSecureDesktopUnavailable,
    kRateLimited,
    kInternalError,
};

struct StepUpProof {
    std::string operator_id;
    std::string device_fingerprint;
    std::string challenge_id;
    std::string signed_proof;
    std::uint64_t issued_at_unix;
};

struct PrivilegedGrantToken {
    std::string token_id;
    std::string session_id;
    std::string operator_id;
    std::uint64_t expires_at_unix;
    std::string token_signature;
};

struct PrivilegedRequest {
    std::string session_id;
    StepUpProof step_up_proof;
    std::string reason_code;
};

struct PrivilegedRequestResult {
    bool accepted;
    PrivilegedError error;
    CapabilityLevel granted_level;
    PrivilegedGrantToken grant_token;
};

struct UacConsentAction {
    std::string session_id;
    std::string token_id;
    PrivilegedDecision decision; // kAllow or kDeny expected from controller
    std::string uac_prompt_id;
};

struct UacConsentResult {
    bool applied;
    PrivilegedError error;
    PrivilegedDecision final_decision;
};

} // namespace redclaw::service
```

## Broker Interface
```cpp
namespace redclaw::service {

class IPrivilegedControlBroker {
public:
    virtual ~IPrivilegedControlBroker() = default;

    virtual PrivilegedRequestResult requestPrivilegedControl(
        const PrivilegedRequest& request) = 0;

    virtual UacConsentResult confirmUacConsent(
        const UacConsentAction& action) = 0;

    virtual bool revokePrivilegedControl(
        std::string_view session_id,
        std::string_view reason_code) = 0;

    virtual CapabilityLevel currentCapability(
        std::string_view session_id) const = 0;
};

} // namespace redclaw::service
```

## Event Hooks (M10)
- `onPrivilegedRequestEvaluated(session_id, operator_id, decision, error)`
- `onPrivilegedCapabilityChanged(session_id, before, after, reason_code)`
- `onUacConsentHandled(session_id, prompt_id, final_decision, error)`

## Deterministic State Machine

### States
- `S0_IDLE`
- `S1_STANDARD_ACTIVE`
- `S2_PRIVILEGE_PENDING`
- `S3_FULL_CONTROL_ACTIVE`
- `S4_UAC_PROMPT_ACTIVE`
- `S5_DENIED_LOCKOUT`
- `S6_REVOKED`

### Transition Rules
1. `S0_IDLE -> S1_STANDARD_ACTIVE`
- Trigger: session authenticated and connected.

2. `S1_STANDARD_ACTIVE -> S2_PRIVILEGE_PENDING`
- Trigger: `requestPrivilegedControl` accepted for verification.

3. `S2_PRIVILEGE_PENDING -> S3_FULL_CONTROL_ACTIVE`
- Trigger: policy + step-up verification pass and short-lived grant token issued.

4. `S2_PRIVILEGE_PENDING -> S1_STANDARD_ACTIVE`
- Trigger: verification failed or expired.

5. `S3_FULL_CONTROL_ACTIVE -> S4_UAC_PROMPT_ACTIVE`
- Trigger: OS raises UAC prompt and secure-desktop channel is available.

6. `S4_UAC_PROMPT_ACTIVE -> S3_FULL_CONTROL_ACTIVE`
- Trigger: UAC decision handled (`allow` or `deny`) and prompt closes.

7. `S3_FULL_CONTROL_ACTIVE -> S6_REVOKED`
- Trigger: token expires, session disconnect, explicit revoke, or policy update.

8. `S4_UAC_PROMPT_ACTIVE -> S6_REVOKED`
- Trigger: secure-desktop channel failure, timeout, or replay detection.

9. `S2_PRIVILEGE_PENDING -> S5_DENIED_LOCKOUT`
- Trigger: repeated failed attempts beyond threshold.

10. `S5_DENIED_LOCKOUT -> S1_STANDARD_ACTIVE`
- Trigger: lockout cooldown completed and risk status cleared.

## Invariants
- Full-control must never be activated without a valid non-expired token.
- UAC consent action must reference current session + token + prompt id.
- Any verification error fails closed to standard-control.
- Every privileged transition emits one audit event.

## Timeouts and Limits (initial)
- Step-up proof validity: 120 seconds.
- Grant token TTL: 120 seconds.
- UAC action timeout: 30 seconds.
- Failed privileged attempts before lockout: 5.
- Lockout cooldown: 300 seconds.

## Replay and Freshness Checks
- Enforce monotonic nonce per session for privileged requests.
- Reject duplicate `challenge_id` or `token_id`.
- Reject `issued_at_unix` outside allowed clock-skew window.

## Suggested Test Cases
- Unit: all legal transitions from each state.
- Unit: illegal transition rejection matrix.
- Unit: token expiry and replay rejection.
- Integration: authorized UAC approve path and audit event.
- Integration: unauthorized UAC action blocked and lockout behavior.

## Implementation Sequence
1. Create header and stubs under `src/service/include` and `src/service/src`.
2. Implement state machine table and transition validator.
3. Add broker policy validator hooks for M03 identity proofs.
4. Wire M10 audit hooks.
5. Add unit tests for transition and token handling.
