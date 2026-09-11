# Test Matrix

## Test Levels
- Unit tests: pure logic and protocol safety.
- Integration tests: multi-process peer sessions.
- End-to-end tests: real host/controller installation flow.
- Resilience tests: network chaos and process restarts.
- Security tests: replay, tamper, unauthorized access attempts.
- Performance tests: latency/fps/bitrate/cpu/memory.

## Priority Scenarios

| # | Scenario | Owner Module(s) | Owner Task(s) |
|---|----------|----------------|---------------|
| 1 | Offline signaling happy path | M02, M01, M09 | M02-T01/T02/T03, M01-T03, M09-T01 |
| 2 | Wrong passphrase path | M02, M03 | M02-T02, M03-T01 |
| 3 | NAT stress | M01 | M01-T01/T03 |
| 4 | Boot-before-login unattended access | M07 | M07-T01/T02 |
| 5 | Login handoff continuity | M07, M06, M09 | M07-T03, M06-T04, M09-T02 |
| 6 | Long-run stability | M01, M09, M10 | M01-T03, M09-T02, M10-T02 |
| 7 | Remote UAC confirmation (authorized) | M07, M06 | M07-T04, M06-T04 |
| 8 | Remote UAC confirmation (unauthorized) | M07, M06, M10 | M07-T04, M06-T03, M10-T01 |
| 9 | DirectX game window visibility | M04, M05 | M04-T04, M05-T03 |

### Scenario Details

1. **Offline signaling happy path**
   - Offer generated, transferred manually, answer applied, session established.
   - Pass criteria: session setup completes within KPI baseline.

2. **Wrong passphrase path**
   - Decrypt failure, no partial session state leak.
   - Pass criteria: error returned, zero bytes of session state exposed.

3. **NAT stress**
   - Full cone and port-restricted NAT direct success.
   - Symmetric NAT fallback to TURN.
   - Pass criteria: connection success rate >= 95% on full-cone, >= 80% on port-restricted; TURN fallback completes within 15s.

4. **Boot-before-login unattended access (Windows)**
   - Host service starts after reboot before interactive login.
   - Session can connect to login screen under policy.
   - Pass criteria: service registered and reachable within 30s of boot completion.

5. **Login handoff continuity**
   - Session continuity or controlled reconnect during switch from secure desktop to user desktop.
   - Pass criteria: reconnect completes within 5s; no input event loss during transition.

6. **Long-run stability**
   - 8-hour session with periodic network jitter and packet loss.
   - Pass criteria: no unbounded memory growth, reconnect within 5s after each jitter event, zero crash.

7. **Remote UAC confirmation (authorized)**
   - Full-control policy enabled, strict verification passes, operator can view and confirm UAC prompt remotely.
   - Pass criteria: UAC consent delivered and confirmed; audit record includes operator_id + timestamp + action.
   - Validation runner: `scripts/service/run-m07-t04-uac-e2e-validation.ps1`
   - Runbook: `docs/testing/m07-t04-uac-e2e-runbook.md`

8. **Remote UAC confirmation (unauthorized)**
   - Without required verification/policy, UAC consent input is denied and security audit event is generated.
   - Pass criteria: input denied with error code, audit event logged with denial reason, no partial consent state.
   - Validation runner: `scripts/service/run-m07-t04-uac-e2e-validation.ps1`
   - Runbook: `docs/testing/m07-t04-uac-e2e-runbook.md`

9. **DirectX game window visibility**
   - Remote stream shows DirectX game window content without persistent black screen in supported capture modes.
   - Windowed and borderless-fullscreen scenarios are validated on baseline GPU/driver matrix.
   - Pass criteria: no persistent black frame (> 3s) over 10-min capture session; backend fallback to WGC triggers within 5 failed frames.

## P0 Desktop Stream Local Simulation Matrix

Run the deterministic offline matrix before requesting a two-LAN reproduction:

```powershell
.\scripts\service\run-local-desktop-stream-simulation-matrix.ps1
```

The runner builds only the focused test targets and writes a versioned JSON result plus build/CTest logs under `build/reports/local-stream-simulation-<timestamp>/`. The fixed seed is recorded in the result so a failure can be replayed exactly.

| Scenario | Actual components exercised | Required invariant |
|----------|-----------------------------|--------------------|
| Static source | retained IDR, transport estimator, congestion controller, control-heartbeat clock | Zero new media is not loss; stale receiver pressure cannot reduce the rate; control remains live. |
| Remote log after reconnect | control epoch guard, bounded remote-log request/chunk/complete messages | A retained request is restamped for the replacement epoch; old-epoch replay is rejected and the new snapshot completes. |
| Viewport replay after reconnect | control epoch guard and sticky viewport request | The same viewport is restamped and resent three times in the replacement epoch, so one transient accepted-but-lost send cannot leave Host without the current display target. |
| Forced active source | v3 fragment serialization/parsing, transport feedback, estimator, reassembler | Changed frames complete continuously and drain in-flight metadata. |
| Seeded random fragment loss | real fragment boundaries, dependency gate, latest complete IDR recovery | Incomplete/dependent P frames never render; a later complete IDR restores output. |
| Weak network | production token bucket and congestion controller | Traffic stays bounded, pressure backs off, and the pacing floor is respected. |
| Capacity step down/up | fresh queue evidence and stable-window probing | A sudden drop reduces pacing; sustained recovery probes upward without an immediate oscillation. |
| Open channels without first media | production stream-health classifier and typed Debug status | After both required channels open, zero first-media progress fails at 10 seconds as capture/encode/transmit/receive/playback startup stall; a real delivered frame clears the gate. |
| Revision/age isolation | payload-free sent metadata ring and transport estimator | Old feedback may retire only old in-flight metadata and cannot alter the current rate revision. |
| DHT loss/delay | encrypted `DhtRendezvousClient` records over a deterministic fault store | Offer, answer, and acknowledgement converge after dropped publish/fetch attempts and delay. |
| DHT out-of-order record | DHT revision/generation contract | A late old record cannot roll back the visible revision or ICE generation. |
| DHT/ICE negotiation state | production connection-negotiation state machine | Persistent offers, duplicate/reordered delivery, late candidates, failed transports, and bounded reconnect converge safely. |
| Failed DHT generation isolation | production recovery/generation policy | A Controller repair cannot re-adopt the failed Host generation; its fresh request replaces an answered but unconnected Host attempt and waits for a strictly newer offer. |
| Real codec roundtrip | FFmpeg encoder/decoder across three resolutions | Low-latency encoded payloads decode, and resolution changes reset dependency state without stale output. |
| Host post-session request takeover | persistent Host offer policy and negotiation coordinator | A healthy session rejects a competing request; after disconnect/reset, a fresh Controller request replaces standby and advances to a newer Host generation. |

The local dual-GUI runner consumes the same `health` value from the runtime rates line. It stops immediately on a stable `*_startup_stalled` classification instead of waiting for the outer run timeout and records `failure_classification` plus `first_media_deadline_ms` in `result.json`.

The runner's default 300-second outer limit is deliberately longer than one 90-second ICE attempt so the public-DHT path can exercise one complete failed-generation repair. It still exits as soon as the full media/UI gate is stable.

This matrix deliberately does not claim to emulate public-DHT routing-table reachability, NAT mapping, ICE candidate reachability, GPU-driver behavior, or physical WAN scheduling. Those environment-dependent items remain final same-SHA two-LAN gates. The local matrix is the first-line regression and fault-localization tool; two-LAN runs validate the external environment rather than discover basic state-machine defects.

## KPI Baselines (initial)

| Metric | Target | Regression Gate |
|--------|--------|-----------------|
| Session setup time | <= 10s typical | PR: warn if > 8s; Nightly: fail if > 12s |
| End-to-end input latency | <= 80ms (same-region healthy link) | Nightly: fail if p95 > 100ms |
| Reconnect after transient drop | <= 5s | Nightly: fail if > 8s |
| Memory growth (8-hour run) | No unbounded growth | Nightly: fail if RSS delta > 50 MB |
| Frame capture latency (M04) | <= 16ms per frame (60fps) | Nightly: warn if p95 > 20ms |
| NAT direct connection rate | >= 95% full-cone, >= 80% port-restricted | Nightly: alert on regression |

Baseline values are initial targets. Review and update at each milestone boundary (M1-M5).

For GUI/input/Agent concurrent performance, the effective 2026-09-11 contract is
[Product Performance Baseline v1](product-performance-baseline-v1.md). It keeps the
100 ms input, 250 ms running-heartbeat/ACK and 90% paired-FPS thresholds for a
Release-optimized, paced representative workload, adds a 15 FPS absolute floor, and
separates that product decision from the Debug unpaced 1 MiB isolation stress. Existing
Debug failures remain recorded. `DEV-FUNC-1` now passes on real-path identity and correctness;
the current hardware-calibrated performance observations are within their alert lines but do
not participate in feature-development pass/fail. `PB-REL-1` remains unverified for the release
candidate decision. Correctness failures remain blocking in every phase.

## CI Gates
- **PR gate:** unit + selected integration smoke.
- **Nightly gate:** full integration + resilience subset + KPI regression checks.
- **Release gate:** full matrix + manual validation checklist (see `docs/testing/release-validation-checklist.md`).
