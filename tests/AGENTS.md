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
- Admin-gated or reboot/dual-machine evidence belongs in scripts and runbooks, not in default unit tests.

## Assertion Style

- New test code should use GoogleTest `EXPECT_*` and `ASSERT_*` macros when adding new files or substantially rewriting a test.
- Existing custom `expect_true()` tests may be extended narrowly when that avoids noisy migration churn.
- Do not mix assertion frameworks inside a single file unless the task is explicitly migrating it.

## Recommended Commands

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
- For stream work, prioritize frame counters, host/controller logs, visible controller output, and two-machine artifacts over isolated module coverage.
