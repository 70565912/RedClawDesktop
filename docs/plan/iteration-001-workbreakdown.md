# Iteration 001 Work Breakdown (Execution Ready)

## Goal
Deliver a functional Windows-first MVP:
- Offline signaling based P2P connection bootstrap.
- Basic remote desktop control.
- Boot-time unattended host service capability.

## Step-by-Step Tasks

### Phase A - Foundation
1. A01 Create mono-repo scaffold and CI [in-progress]
- Output: build scripts, lint, test harness.
- Tests: CI pipeline dry run.

2. A02 Define protobuf/json schemas for signaling/session messages [in-progress]
- Output: versioned schema package.
- Tests: schema compatibility tests.

3. A03 Implement structured logging and trace IDs
- Output: log SDK and redaction rules.
- Tests: secret redaction unit tests.

### Phase B - Connectivity + Offline signaling
4. B01 Implement M01 candidate lifecycle and ICE state machine wrapper
- Tests: candidate parser unit tests, local dual-peer integration.

5. B02 Implement M02 offer blob encryption/decryption and QR serialization
- Tests: wrong passphrase, corrupted blob, version mismatch cases.

6. B03 Integrate B01+B02 into two-end bootstrap flow
- Tests: manual copy/paste e2e on two machines.

### Phase C - Security and session control
7. C01 Implement M03 handshake and peer identity checks
- Tests: replay/tamper rejection tests.

8. C02 Implement M09 deterministic session lifecycle manager
- Tests: transition table coverage and fault injection.

### Phase D - Desktop core
9. D01 Implement M04 Windows capture and encoder pipeline
- Tests: long-run capture stability and memory profile.

10. D02 Implement M05 decode/render path on controller
- Tests: dynamic resolution and frame-drop tolerance.

11. D03 Implement M06 input injection with permission policy
- Tests: blocked/allowed event policy tests.

### Phase E - Unattended access
12. E01 Implement M07 service installer and service host bootstrap
- Tests: install/start/stop/uninstall flow.

13. E02 Implement pre-login session path and user-session handoff
- Tests: reboot -> pre-login connect -> login handoff scenario.

### Phase F - Productivity features
14. F01 Implement M08 clipboard sync (text first)
- Tests: bidirectional sync and conflict behavior.

15. F02 Implement M08 resumable file transfer
- Tests: interruption and resume checksum validation.

### Phase G - Hardening
16. G01 Implement M10 diagnostics bundle and connectivity debug panel
- Tests: bundle generation and sensitive field redaction.

17. G02 Run full matrix and close release checklist
- Tests: full `docs/testing/test-matrix.md` gates.

### Phase H - Portable Companion (Mobile/Tablet)
18. H01 Implement M11 Android portable session capability model
- Tests: policy deny/allow unit tests.

19. H02 Implement Android status dashboard data path
- Tests: integration tests for artifact/log fetch and snapshot freshness.

20. H03 Implement guarded command preset execution flow and produce mac/web mini planning docs
- Tests: confirmation gate and high-risk action policy tests; planning doc review checklist.

## Tracking Rule
After finishing each step:
- Update corresponding task in `docs/runtime/MODULE_KANBAN.md` (status + Updated date).
- Append phase summary to `docs/logs/DEVLOG.md` for significant milestones.
- Mark this file step as done/in-progress/blocked.

## Pre-Phase Spikes (must complete before Phase B/D)
- `M01-T00`: libdatachannel peer-loopback PoC (validates P2P stack for Phase B).
- `M04-T00`: DDA minimum capture PoC (validates capture stack for Phase D).
- `X00-T01`: GitHub Actions CI pipeline (gate criterion for foundation→implementation).
