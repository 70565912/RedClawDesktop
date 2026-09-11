# M07-T04 UAC E2E Runbook

This runbook executes the fourth-batch manual evidence flow for remote UAC confirmation scenarios.

## Current Status and Limitation
- This flow is currently an evidence collection run, not a fully automated UAC E2E assertion runner.
- `scripts/service/run-m07-t04-uac-e2e-validation.ps1` records operator-entered scenario outcomes (`pass|fail|skip`) and writes a JSON report.
- Controller-side user-facing commands for "start remote session", "request full-control", and "submit UAC consent" are not yet exposed as a stable CLI entry in this repository.
- Practical meaning: manual validation can run now, but reproducible machine-asserted controller flow still requires a dedicated harness/tooling task.

## Evidence Attachment Register

Use this table to track which run-id package is attached for closure and whether it is local-only or target-environment evidence.

- pending
  - scope: target-environment
  - run_id: TBD
  - verdict: TBD
  - package_path: `build/reports/m07-t04-runs/<run-id>`
  - notes: required to close M07-T04 per D04/D05 boundary.

## Scope
- Scenario A: authorized UAC approve
- Scenario B: authorized UAC deny/timeout
- Scenario C: unauthorized UAC input block

Checklist baseline:
- docs/testing/e2e-privileged-directx-checklist.md

## Preconditions
- Run PowerShell as Administrator.
- Build is up to date (`build/vs2022-x64/src/Debug/redclaw_desktop.exe` exists).
- Host and controller are ready for remote validation.
- Audit sink is enabled for service logs.

Recommended topology:
- Preferred: two machines (Host/controlled machine + Controller/operator machine).
- Fallback: one machine with VM split (lower confidence, more timing/environment noise).

Controller-side operation source:
- If your team has an existing internal controller app/tooling, use that app to drive: session establish -> full-control request -> UAC allow/deny action.
- If not available, treat this run as blocked for strict controller-path proof and use the development tasks in this document to build a minimal harness first.

## Interactive Run
```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-m07-t04-uac-e2e-validation.ps1
```

## One-Click Admin Launcher
```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/start-m07-t04-uac-e2e-validation-admin.ps1
```

Forward arguments to validation script:
```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/start-m07-t04-uac-e2e-validation-admin.ps1 -NonInteractive -ScenarioAResult pass -ScenarioBResult pass -ScenarioCResult pass -OperatorId test-operator -DeviceId test-device
```

The script asks for `pass|fail|skip` per scenario and writes a JSON report.

Interpretation rule:
- `pass` means you observed expected runtime behavior and verified corresponding host-side audit evidence.
- `fail` means behavior or audit evidence is incorrect/incomplete.
- `skip` means scenario could not be executed in current environment (must include reason and rerun plan).

## Non-Interactive Run
```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-m07-t04-uac-e2e-validation.ps1 -NonInteractive -ScenarioAResult pass -ScenarioBResult pass -ScenarioCResult pass -OperatorId test-operator -DeviceId test-device
```

## Single-Machine Automation Mode (Skip Dual-Machine Steps)
Use this mode when the controller-side dual-machine path is not available in the current environment.

Key behavior:
- `run-m07-t04-uac-e2e-validation.ps1` now supports `-AllowSkipScenarios`.
- A scenario marked `skip` is accepted only if the scenario key is explicitly listed in `-AllowSkipScenarios`.
- Recommended for single-machine runs: allow skip for Scenario A (controller-side authorized allow path), keep Scenario B/C as machine-asserted pass via local helper evidence.

Scenario keys:
- `scenario_a_authorized`
- `scenario_b_deny_timeout`
- `scenario_c_unauthorized_block`

Example (single-machine non-interactive):
```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-m07-t04-uac-e2e-validation.ps1 -NonInteractive -ScenarioAResult skip -ScenarioBResult pass -ScenarioCResult pass -AllowSkipScenarios scenario_a_authorized -SkipReason "dual-machine-controller-path-not-available" -OperatorId test-operator -DeviceId test-device
```

Admin launcher example:
```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/start-m07-t04-uac-e2e-validation-admin.ps1 -NonInteractive -ScenarioAResult skip -ScenarioBResult pass -ScenarioCResult pass -AllowSkipScenarios scenario_a_authorized -SkipReason dual-machine-controller-path-not-available -OperatorId test-operator -DeviceId test-device
```

## Output
Default report path:
- `build/reports/m07-t04-uac-e2e-validation.json`

Key fields:
- `passed`
- `checks.scenario_a_authorized`
- `checks.scenario_b_deny_timeout`
- `checks.scenario_c_unauthorized_block`
- `execution_policy.allow_skip_scenarios`
- `execution_policy.accepted_skips`
- `error`

## Evidence Guidance
- Attach report JSON to handoff log.
- Keep host-side audit snippets that include:
  - `operator_id`
  - `session_id`
  - `decision`
  - `error`
  - `timestamp_unix`
- If any scenario is `fail`/`skip`, include a short reason and rerun plan.
- If single-machine skip mode is used, include `AllowSkipScenarios` and `SkipReason` in handoff notes.

Minimal evidence package per run:
- Validation JSON (`build/reports/m07-t04-uac-e2e-validation.json` or custom path).
- Host audit excerpt for Scenario A/B/C (at least one event per scenario).
- Environment stamp (host OS, controller app/tool version, build commit hash).
- Operator notes for unexpected timing/retry conditions.

## Manual Test Procedure (Operator Checklist)
1. Start host-side runtime in target environment and confirm audit sink availability.
2. Launch controller app/tool and establish session to target host.
3. Execute Scenario A (authorized allow): request full-control, trigger UAC, choose allow, verify protected action succeeds and audit decision is allow.
4. Execute Scenario B (authorized deny/timeout): request full-control, trigger UAC, choose deny or wait timeout, verify protected action is not executed and audit decision is deny/timeout.
5. Execute Scenario C (unauthorized block): use unverified/untrusted policy path, attempt UAC consent input, verify input is blocked and audit records blocked/policy denial.
6. Run validation script and enter scenario outcomes (`pass|fail|skip`) with notes.
7. Archive JSON + audit snippets + environment stamp into handoff evidence.

## Development Tasks (Next Step to Remove Manual Gaps)
If controller-side command path is not available, complete these tasks before claiming fully verifiable E2E status:

1. M07-T04-D01: Windows controller harness CLI (single-machine first)
- Objective: provide a deterministic Windows CLI that drives privileged request and UAC decision actions through existing runtime interfaces.
- Scope:
  - Command actions: `request-full-control`, `uac-allow`, `uac-deny`, `uac-timeout`.
  - Input options: `session-id`, `operator-id`, `device-id`, `token-id`, `uac-prompt-id`, `reason-code`.
  - Output: structured JSON to stdout/file for each action result.
- Deliverables:
  - New harness executable target.
  - Basic usage doc block with command examples.
  - Deterministic unit/integration tests for action parsing and action-to-runtime mapping.
- Acceptance:
  - Harness can issue all required actions and returns explicit success/error codes.
  - Invalid/expired token paths return expected failure signals.

2. M07-T04-D02: Windows host prompt trigger helper
- Objective: make UAC prompt path triggerable in a controlled and repeatable way for tests.
- Scope:
  - Add a host-side test trigger for privileged action flow.
  - Emit stable correlation identifiers for `session_id`, `token_id`, and `uac_prompt_id`.
- Deliverables:
  - Helper entrypoint callable during local test runs.
  - Log/event output schema section in docs.
- Acceptance:
  - Trigger can be invoked repeatedly with consistent correlation IDs visible in output.
  - Failure mode is fail-closed and auditable.

3. M07-T04-D03: Windows audit evidence validator
- Objective: convert manual audit-field checking into machine assertions.
- Scope:
  - Parse host audit records and verify required field presence and scenario decision mapping.
  - Validate A/B/C scenario outcomes against expected decision/error pairs.
- Deliverables:
  - Validation script extension or dedicated validator script.
  - Output report with per-scenario pass/fail and mismatch reason.
- Acceptance:
  - Missing fields or wrong decision mapping fail the run automatically.
  - Report is suitable for handoff attachment without manual reinterpretation.

4. M07-T04-D04: Windows dual-machine runner guide
- Objective: provide reproducible host/controller execution steps for target-environment validation.
- Scope:
  - Explicit role split: Host machine and Controller machine.
  - Ordered execution steps, timing/retry rules, and evidence capture checklist.
- Deliverables:
  - Runbook section with exact step order and troubleshooting notes.
  - Standard evidence package template.
- Acceptance:
  - New operator can follow the guide and complete one full A/B/C run without tribal knowledge.

5. M07-T04-D05: Windows CI/manual boundary definition
- Objective: separate automatable validation from environment-gated validation.
- Scope:
  - CI scope: harness contract/integration checks that do not require real UAC desktop interaction.
  - Manual scope: real secure-desktop/UAC confirmation evidence in target environment.
- Deliverables:
  - Documented test boundary and required evidence for each boundary.
  - Suggested pipeline stage map.
- Acceptance:
  - Team can classify every M07-T04 test as CI or manual without ambiguity.

## Windows Execution Order (Recommended)
1. Implement D01 first to unblock commandable controller actions.
2. Implement D02 to make host prompt triggering deterministic.
3. Implement D03 so evidence checking becomes machine-asserted.
4. Update D04 to enable reproducible dual-machine target runs.
5. Finalize D05 and wire CI/manual reporting expectations.

## D01 Initial Implementation (Windows)
Current executable target:
- `redclaw_m07_controller_harness_cli`

Build target example:
```powershell
cmake --build --preset debug-local --target redclaw_m07_controller_harness_cli
```

Binary path example:
- `build/vs2022-x64/tests/Debug/redclaw_m07_controller_harness_cli.exe`

Usage example (request full-control):
```powershell
build/vs2022-x64/tests/Debug/redclaw_m07_controller_harness_cli.exe --action request-full-control --session-id m07-dev-session --operator-id m07-operator --device-id m07-device
```

Usage example (UAC allow):
```powershell
build/vs2022-x64/tests/Debug/redclaw_m07_controller_harness_cli.exe --action uac-allow --session-id m07-dev-session --operator-id m07-operator --device-id m07-device --uac-prompt-id m07-prompt-allow
```

Usage example (UAC deny):
```powershell
build/vs2022-x64/tests/Debug/redclaw_m07_controller_harness_cli.exe --action uac-deny --session-id m07-dev-session --operator-id m07-operator --device-id m07-device --uac-prompt-id m07-prompt-deny
```

Usage example (UAC timeout fail-close path):
```powershell
build/vs2022-x64/tests/Debug/redclaw_m07_controller_harness_cli.exe --action uac-timeout --session-id m07-dev-session --operator-id m07-operator --device-id m07-device --uac-prompt-id m07-prompt-timeout
```

Optional JSON artifact output:
```powershell
build/vs2022-x64/tests/Debug/redclaw_m07_controller_harness_cli.exe --action uac-allow --output-json-path build/reports/m07-d01-uac-allow.json
```

## D02 Initial Implementation (Windows)
Current executable target:
- `redclaw_m07_host_prompt_trigger_helper`

Build target example:
```powershell
cmake --build --preset debug-local --target redclaw_m07_host_prompt_trigger_helper
```

Binary path example:
- `build/vs2022-x64/tests/Debug/redclaw_m07_host_prompt_trigger_helper.exe`

Usage example (raise prompt and keep full-control):
```powershell
build/vs2022-x64/tests/Debug/redclaw_m07_host_prompt_trigger_helper.exe --session-id m07-trigger-session --operator-id m07-operator --device-id m07-device --uac-prompt-id m07-prompt-raised --prompt-event raised
```

Usage example (trigger timeout fail-close):
```powershell
build/vs2022-x64/tests/Debug/redclaw_m07_host_prompt_trigger_helper.exe --session-id m07-trigger-session --operator-id m07-operator --device-id m07-device --uac-prompt-id m07-prompt-timeout --prompt-event timeout
```

Usage example (trigger unavailable fail-close with reason):
```powershell
build/vs2022-x64/tests/Debug/redclaw_m07_host_prompt_trigger_helper.exe --session-id m07-trigger-session --operator-id m07-operator --device-id m07-device --uac-prompt-id m07-prompt-unavailable --prompt-event unavailable --unavailable-reason secure_desktop_transport_lost
```

Optional JSON artifact output:
```powershell
build/vs2022-x64/tests/Debug/redclaw_m07_host_prompt_trigger_helper.exe --prompt-event raised --output-json-path build/reports/m07-d02-prompt-raised.json
```

## D03 Initial Implementation (Windows)
Current script:
- `scripts/service/validate-m07-t04-uac-audit-evidence.ps1`

Purpose:
- Parse scenario evidence JSON files and automatically validate required fields plus scenario decision mapping.
- Required fields checked per event: `operator_id`, `session_id`, `decision`, `error`, `timestamp_unix`.
- Scenario mapping checks:
  - Scenario A expects allow-applied evidence.
  - Scenario B expects deny-applied or timeout/closed fail-close evidence.
  - Scenario C expects blocked decision with policy/security denial reason.

Usage example:
```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/validate-m07-t04-uac-audit-evidence.ps1 -ScenarioAAuditPath build/reports/m07-d01-scenario-a.json -ScenarioBAuditPath build/reports/m07-d02-scenario-b.json -ScenarioCAuditPath build/reports/m07-d02-scenario-c.json -ExpectedSessionId m07-dev-session -ExpectedOperatorId m07-operator -OutputJsonPath build/reports/m07-d03-audit-validation.json
```

Output:
- Validation report JSON (default): `build/reports/m07-t04-uac-audit-evidence-validation.json`
- Key report fields:
  - `passed`
  - `scenarios[*].passed`
  - `scenarios[*].checks`
  - `scenarios[*].details`
  - `error`

## D04 Initial Implementation (Windows Dual-Machine Runner Guide)

### Machine Roles
- Machine A (Host, controlled): Windows machine that runs host-side runtime and emits privileged audit events.
- Machine B (Controller, operator): Windows machine that runs D01 harness CLI and drives full-control/UAC decision actions.

### Topology Note
- Preferred topology is two physical Windows machines connected over real network paths.
- Both machines can be behind different home/office routers; treat this as target-environment validation when connectivity and timing are stable.

### Prerequisites
- Host and Controller clocks are within 60 seconds.
- Host build artifacts are up to date.
- Shared evidence location is available (network share or copied artifacts).
- Operator uses fixed identifiers for one run: `session_id`, `operator_id`, `device_id`.

### Ordered Execution Steps
1. Machine A (Host): define this run's fixed identifiers (same values must be used on both machines).
```powershell
$SessionId = "m07-dev-session"
$OperatorId = "m07-operator"
$DeviceId = "m07-device"
```

2. Machine B (Controller): generate Scenario A evidence (`uac-allow`).
```powershell
build/vs2022-x64/tests/Debug/redclaw_m07_controller_harness_cli.exe --action uac-allow --session-id m07-dev-session --operator-id m07-operator --device-id m07-device --uac-prompt-id m07-prompt-allow --output-json-path build/reports/m07-d01-scenario-a.json
```

3. Machine A (Host): generate Scenario B evidence (`timeout` fail-close path).
```powershell
build/vs2022-x64/tests/Debug/redclaw_m07_host_prompt_trigger_helper.exe --session-id m07-dev-session --operator-id m07-operator --device-id m07-device --uac-prompt-id m07-prompt-timeout --prompt-event timeout --output-json-path build/reports/m07-d02-scenario-b.json
```

4. Machine A (Host): generate Scenario C evidence (`unavailable` blocked path).
```powershell
build/vs2022-x64/tests/Debug/redclaw_m07_host_prompt_trigger_helper.exe --session-id m07-dev-session --operator-id m07-operator --device-id m07-device --uac-prompt-id m07-prompt-unavailable --prompt-event unavailable --unavailable-reason secure_desktop_transport_lost --output-json-path build/reports/m07-d02-scenario-c.json
```

5. Machine B (Controller): run D03 validator on A/B/C artifacts.
```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/validate-m07-t04-uac-audit-evidence.ps1 -ScenarioAAuditPath build/reports/m07-d01-scenario-a.json -ScenarioBAuditPath build/reports/m07-d02-scenario-b.json -ScenarioCAuditPath build/reports/m07-d02-scenario-c.json -ExpectedSessionId m07-dev-session -ExpectedOperatorId m07-operator -OutputJsonPath build/reports/m07-d03-audit-validation.json
```

6. Machine A (Host): run admin-gated manual evidence collector and record scenario notes.
```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/run-m07-t04-uac-e2e-validation.ps1 -SessionId m07-dev-session -OperatorId m07-operator -DeviceId m07-device
```

7. Machine A (Host): archive final evidence package to a unique run directory (prevents overwrite ambiguity).
```powershell
$RunId = "m07-t04-" + (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/archive-m07-t04-uac-evidence.ps1 -RunId $RunId -Strict
```

8. Machine B (Controller) or shared handoff workspace: attach archive directory and manifest in handoff:
```text
build/reports/m07-t04-runs/<run-id>/
- evidence-manifest.json
- m07-d01-scenario-a.json
- m07-d02-scenario-b.json
- m07-d02-scenario-c.json
- m07-d03-audit-validation.json
- m07-t04-uac-e2e-validation.json
```

### Retry Rules
- If D01 returns non-zero: re-run with a fresh `challenge-id`.
- If D02 returns non-zero: switch prompt event from `timeout` to `unavailable` and re-run once.
- If D03 fails: inspect `scenarios[*].details`, fix artifact mismatch, and re-run D03 only.
- Maximum retry policy: 2 retries per scenario before marking run blocked.

### Standard Evidence Package Template
- `scenario_a_artifact`: JSON from D01 `uac-allow`.
- `scenario_b_artifact`: JSON from D02 `timeout` or `unavailable`.
- `scenario_c_artifact`: JSON from D02 `unavailable`.
- `audit_validation_report`: JSON from D03.
- `manual_evidence_report`: JSON from `run-m07-t04-uac-e2e-validation.ps1`.
- `archive_manifest`: JSON from `archive-m07-t04-uac-evidence.ps1` (contains SHA256 and copied/missing files).
- `environment_stamp`: host OS version, controller OS version, commit hash, timestamp range.
- `operator_notes`: observed anomalies, retry count, rerun plan (if any).

### Dual-Machine Command Sequence (Example)
Host machine (Scenario B timeout):
```powershell
build/vs2022-x64/tests/Debug/redclaw_m07_host_prompt_trigger_helper.exe --session-id m07-dev-session --operator-id m07-operator --device-id m07-device --uac-prompt-id m07-prompt-timeout --prompt-event timeout --output-json-path build/reports/m07-d02-scenario-b.json
```

Controller machine (Scenario A allow):
```powershell
build/vs2022-x64/tests/Debug/redclaw_m07_controller_harness_cli.exe --action uac-allow --session-id m07-dev-session --operator-id m07-operator --device-id m07-device --uac-prompt-id m07-prompt-allow --output-json-path build/reports/m07-d01-scenario-a.json
```

Controller machine (D03 validation):
```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/service/validate-m07-t04-uac-audit-evidence.ps1 -ScenarioAAuditPath build/reports/m07-d01-scenario-a.json -ScenarioBAuditPath build/reports/m07-d02-scenario-b.json -ScenarioCAuditPath build/reports/m07-d02-scenario-c.json -ExpectedSessionId m07-dev-session -ExpectedOperatorId m07-operator -OutputJsonPath build/reports/m07-d03-audit-validation.json
```

## D05 Initial Implementation (CI/Manual Boundary Definition)

### CI Scope (Automatable)
- Build targets:
  - `redclaw_m07_controller_harness_cli`
  - `redclaw_m07_host_prompt_trigger_helper`
- Integration/contract suites already in CI-friendly path:
  - `redclaw_m07_uac_consent_signal_producer_integration_tests`
  - `redclaw_m07_session_uac_consent_ingress_router_integration_tests`
  - `redclaw_m07_uac_prompt_runtime_coordinator_integration_tests`
- Evidence structure validation:
  - D03 script validation on generated JSON artifacts.

### Manual Scope (Environment-Gated)
- Real secure-desktop/UAC desktop interaction in target environment.
- Admin-gated runbook execution:
  - `scripts/service/run-m07-t04-uac-e2e-validation.ps1`
- Final PASS evidence collection for task closure in target environment.

### Single-Machine Fallback Scope (Manual, Constrained)
- Use when two-machine host/controller setup is temporarily unavailable.
- Allowed fallback: skip dual-machine-only scenarios through `-AllowSkipScenarios` with explicit reason.
- Required: remaining non-skipped scenarios must be `pass`; D03 audit mapping remains machine-asserted.
- Output classification: `PASS_WITH_ACCEPTED_SKIPS` for interim progress tracking, not a replacement for final target-environment dual-machine closure.

### Decision Matrix
- If test requires real Windows secure desktop context: Manual.
- If test only validates contract/state machine/JSON evidence mapping: CI.
- If test needs Administrator but not real secure desktop: Manual by default, can be promoted later with dedicated runner host.
- If dual-machine topology is unavailable: run single-machine fallback with explicit skip policy and mark closure scope as interim.

### Pipeline Stage Map (Recommended)
1. Stage `build-and-contract` (CI): build D01/D02 targets and run deterministic integration tests.
2. Stage `evidence-validation` (CI): run D03 on generated artifacts.
3. Stage `target-env-uac-e2e` (Manual gated): execute runbook on target Host/Controller environment and publish evidence package.

### Release Gate Recommendation
- Merge gate for M07-T04 development branch:
  - CI stage 1 + 2 must pass.
- Milestone closure gate for M07-T04:
  - Manual stage 3 PASS evidence package required.

## Planning Status
- This document now serves as the Windows-first development task baseline for M07-T04 verification tooling.
