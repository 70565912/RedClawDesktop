# Tests Directory Agent Guide

Applies to `tests/**`.

## Test Layout

- Tests live under `tests/<module>/` for module tests and `tests/integration/` for cross-module tests.
- File names should use `<feature>_tests.cpp`.
- Add new executables and `add_test(...)` entries in `tests/CMakeLists.txt`.
- Link each test to the smallest required set of module libraries.
- Apply `redclaw_apply_warnings(<target>)` to every test target.

## Unit vs Integration Split

- Current project priority is dual-machine desktop stream proof. Do not add broad module test matrices unless they directly unblock or protect that proof.
- Unit-style tests should avoid OS/admin/network dependencies and use in-memory adapters or deterministic fakes.
- Integration/e2e tests should include `_integration_` or `_e2e_` in the target/test name so they can be excluded from the unit gate.
- Cases needing human consent, real account login, physical keys, special desktop/topology setup or visual judgment must not be registered in the automated CTest sequence or required for version release. Keep developer-invoked tools/runbooks separate; do not register them as disabled/skipped placeholders.
- Execution location is independent of the unit/integration name: follow [the test matrix](../docs/testing/test-matrix.md). Most OS and Host/Controller integration is locally automatable; only real external-network risks require cross-LAN repetition.
- Prefer isolated unattended fixtures with deterministic assertions and machine-readable results. Do not add manual confirmations where an actual result can be asserted. Mixed executables must filter out developer-selected manual cases at CTest registration; an unexpected automatic skip is a test/environment error, not a deferred manual gate.

## Assertion Style

- New test code should use GoogleTest `EXPECT_*` and `ASSERT_*` macros when adding new files or substantially rewriting a test.
- Existing custom `expect_true()` tests may be extended narrowly when that avoids noisy migration churn.
- Do not mix assertion frameworks inside a single file unless the task is explicitly migrating it.

## Recommended Commands

For ordinary iterations, select affected areas once (replace the directory with the configured build tree):

```powershell
.\scripts\service\run-local-validation.ps1 -Area input,workspace -BuildDirectory build/ninja-x64
```

This builds only selected test targets, deduplicates suites, runs serially and writes aggregate plus case-level results. The broad commands below are checkpoints, not prerequisites for every remote diagnosis.

Build unit targets:

```powershell
cmake --build --preset debug-local --target redclaw_tests_unit
```

Run unit CTest gate:

```powershell
ctest --test-dir build/vs2022-x64 -C Debug --output-on-failure -E "_integration_tests|_e2e_tests"
```

Run full local sweep only for release-style checkpoints or broad shared behavior changes:

```powershell
cmake --build --preset debug-tests-local
ctest --test-dir build/vs2022-x64 -C Debug --output-on-failure
```

## Evidence Rules

- Capture exact command and pass/fail summary in final responses.
- If skipping integration/e2e validation, state why and name the remaining risk.
- Keep generated reports under `build/reports/` or task-specific generated paths.
- For actual stream integration, require real media counters and visible output; pure parser/state tests need no desktop/peer. Reuse valid local evidence instead of repeating the full feature matrix across LANs. One attributable application result proves consumption; extra peer readback is diagnostic only.
